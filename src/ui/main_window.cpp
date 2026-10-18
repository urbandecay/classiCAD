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
#include "../core/serialization/document_serializer.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMainWindow>
#include <QPushButton>
#include <QPixmap>
#include <QSaveFile>
#include <QShortcut>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QSplitter>
#include <QTabWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVariant>
#include <QWidget>
#include <QtMath>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
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
        restoreOutlinerClipboard();
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
    QString outlinerClipboardFilePath() const
    {
        const QString dataDirectory = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
        if (dataDirectory.isEmpty()) return {};
        return QDir(dataDirectory).filePath(
            QStringLiteral("outliner-copybuffer.json"));
    }

    bool persistOutlinerClipboard()
    {
        const bool collectionClipboard =
            !outlinerCollectionClipboardCollections_.isEmpty();
        const QVector<SceneObject> &sourceObjects = collectionClipboard
            ? outlinerCollectionClipboardObjects_ : outlinerObjectClipboard_;
        const QVector<DocumentMaterial> &sourceMaterials = collectionClipboard
            ? outlinerCollectionClipboardMaterials_
            : outlinerObjectClipboardMaterials_;
        if ((!collectionClipboard && sourceObjects.isEmpty()) ||
            (collectionClipboard &&
             outlinerCollectionClipboardCollections_.isEmpty()) ||
            (collectionClipboard &&
             outlinerCollectionClipboardRootIds_.isEmpty())) {
            return false;
        }

        Document clipboardDocument;
        Document::Snapshot snapshot = clipboardDocument.snapshot();
        snapshot.objects = sourceObjects;
        snapshot.materials = sourceMaterials;
        QSet<quint64> clipboardCollectionIds;
        if (collectionClipboard) {
            snapshot.collections = outlinerCollectionClipboardCollections_;
            for (const DocumentCollection &collection : snapshot.collections) {
                clipboardCollectionIds.insert(collection.id.value());
            }
            for (DocumentCollection &collection : snapshot.collections) {
                QVector<CollectionId> parents;
                QVector<CollectionParentOrder> parentOrders;
                for (const CollectionId parentId : collection.parentIds) {
                    if (!clipboardCollectionIds.contains(parentId.value())) {
                        continue;
                    }
                    parents.append(parentId);
                    const auto order = std::find_if(
                        collection.parentOrders.cbegin(),
                        collection.parentOrders.cend(),
                        [parentId](const CollectionParentOrder &candidate) {
                            return candidate.parentId == parentId;
                        });
                    if (order != collection.parentOrders.cend()) {
                        parentOrders.append(*order);
                    }
                }
                collection.parentIds = std::move(parents);
                collection.parentOrders = std::move(parentOrders);
                if (collection.parentIds.isEmpty()) {
                    collection.rootOrder = qMax(0, collection.rootOrder);
                } else {
                    collection.rootOrder = -1;
                }
            }
            for (SceneObject &object : snapshot.objects) {
                object.collectionIds.erase(
                    std::remove_if(object.collectionIds.begin(),
                                   object.collectionIds.end(),
                                   [&clipboardCollectionIds](
                                       CollectionId collectionId) {
                        return !clipboardCollectionIds.contains(
                            collectionId.value());
                    }),
                    object.collectionIds.end());
                object.instanceCollectionId = CollectionId::invalid();
                object.instanceTransform = identityAffineTransform3D;
            }
        } else {
            for (SceneObject &object : snapshot.objects) {
                object.collectionIds.clear();
                object.instanceCollectionId = CollectionId::invalid();
                object.instanceTransform = identityAffineTransform3D;
            }
        }
        for (const SceneObject &object : snapshot.objects) {
            if (object.id.isValid() &&
                object.id.value() < std::numeric_limits<quint64>::max()) {
                snapshot.nextObjectValue = qMax(
                    snapshot.nextObjectValue, object.id.value() + 1);
            }
        }
        for (const DocumentCollection &collection : snapshot.collections) {
            if (collection.id.isValid() &&
                collection.id.value() < std::numeric_limits<quint64>::max()) {
                snapshot.nextCollectionValue = qMax(
                    snapshot.nextCollectionValue, collection.id.value() + 1);
            }
        }
        clipboardDocument.restoreSnapshot(snapshot);

        const auto serializeIdentityMap = [](
            const QHash<quint64, QString> &identities) {
            QJsonArray result;
            for (auto it = identities.cbegin(); it != identities.cend(); ++it) {
                result.append(QJsonObject{
                    {QStringLiteral("id"), QString::number(it.key())},
                    {QStringLiteral("identity"), it.value()}});
            }
            return result;
        };
        QJsonArray instanceReferences;
        for (const SceneObject &object : sourceObjects) {
            if (!object.id.isValid() || !object.instanceCollectionId.isValid()) {
                continue;
            }
            QJsonArray instanceTransform;
            for (const double value : object.instanceTransform) {
                instanceTransform.append(value);
            }
            instanceReferences.append(QJsonObject{
                {QStringLiteral("object_id"),
                 QString::number(object.id.value())},
                {QStringLiteral("collection_id"),
                 QString::number(object.instanceCollectionId.value())},
                {QStringLiteral("instance_transform"), instanceTransform}});
        }
        const QHash<quint64, QString> &anchorIdentities = collectionClipboard
            ? outlinerCollectionClipboardAnchorIdentities_
            : outlinerObjectClipboardAnchorIdentities_;
        const QHash<quint64, QString> &collectionIdentities =
            collectionClipboard
                ? outlinerCollectionClipboardCollectionIdentities_
                : outlinerObjectClipboardCollectionIdentities_;
        QJsonObject payload{
            {QStringLiteral("format"),
             QStringLiteral("classiCAD-outliner-copybuffer")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("kind"), collectionClipboard
                 ? QStringLiteral("collection") : QStringLiteral("object")},
            {QStringLiteral("document"), documentToJson(clipboardDocument)},
            {QStringLiteral("anchor_identities"),
             serializeIdentityMap(anchorIdentities)},
            {QStringLiteral("collection_identities"),
             serializeIdentityMap(collectionIdentities)},
            {QStringLiteral("instance_collection_references"),
             instanceReferences}};
        if (collectionClipboard) {
            QJsonArray rootCollectionIds;
            for (const CollectionId rootId :
                 outlinerCollectionClipboardRootIds_) {
                if (!rootId.isValid() ||
                    !clipboardCollectionIds.contains(rootId.value())) {
                    return false;
                }
                rootCollectionIds.append(QString::number(rootId.value()));
            }
            payload.insert(QStringLiteral("root_collection_ids"),
                           rootCollectionIds);
            if (outlinerCollectionClipboardRootIds_.size() == 1) {
                payload.insert(QStringLiteral("root_collection_id"),
                               QString::number(
                                   outlinerCollectionClipboardRootIds_.first()
                                       .value()));
            }
        }
        const QByteArray serialized =
            QJsonDocument(payload).toJson(QJsonDocument::Compact);
        constexpr qsizetype maximumClipboardBytes = 512 * 1024 * 1024;
        if (serialized.isEmpty() ||
            serialized.size() > maximumClipboardBytes) {
            return false;
        }
        const QString path = outlinerClipboardFilePath();
        if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(serialized) != serialized.size()) {
            file.cancelWriting();
            return false;
        }
        return file.commit();
    }

    void restoreOutlinerClipboard()
    {
        const QString path = outlinerClipboardFilePath();
        if (path.isEmpty()) return;
        QFile file(path);
        constexpr qint64 maximumClipboardBytes = 512LL * 1024 * 1024;
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
            file.size() > maximumClipboardBytes) {
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(
            file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !json.isObject()) {
            return;
        }
        const QJsonObject payload = json.object();
        if (payload.value(QStringLiteral("format")).toString() !=
                QLatin1String("classiCAD-outliner-copybuffer") ||
            payload.value(QStringLiteral("version")).toInt() != 1) {
            return;
        }
        const QString kind = payload.value(QStringLiteral("kind")).toString();
        const bool collectionClipboard = kind == QLatin1String("collection");
        if (!collectionClipboard && kind != QLatin1String("object")) return;

        Document clipboardDocument;
        if (!documentFromJson(payload.value(QStringLiteral("document")),
                              &clipboardDocument) ||
            (collectionClipboard
                 ? clipboardDocument.collections().isEmpty()
                 : clipboardDocument.objects().isEmpty())) {
            return;
        }
        const auto readIdentityMap = [](
            const QJsonValue &value,
            QHash<quint64, QString> *identities) {
            if (identities == nullptr || !value.isArray() ||
                value.toArray().size() > 100000) {
                return false;
            }
            QHash<quint64, QString> parsed;
            for (const QJsonValue &entryValue : value.toArray()) {
                if (!entryValue.isObject()) return false;
                const QJsonObject entry = entryValue.toObject();
                bool idValid = false;
                const quint64 id = entry.value(QStringLiteral("id"))
                    .toString().toULongLong(&idValid);
                const QString identity = entry.value(
                    QStringLiteral("identity")).toString();
                if (!idValid || id == 0 || identity.isEmpty() ||
                    identity.size() > 4096 || parsed.contains(id)) {
                    return false;
                }
                parsed.insert(id, identity);
            }
            *identities = std::move(parsed);
            return true;
        };
        QHash<quint64, QString> anchorIdentities;
        QHash<quint64, QString> collectionIdentities;
        if (!readIdentityMap(payload.value(QStringLiteral("anchor_identities")),
                             &anchorIdentities) ||
            !readIdentityMap(
                payload.value(QStringLiteral("collection_identities")),
                &collectionIdentities)) {
            return;
        }
        struct ClipboardInstanceReference {
            quint64 collectionId = 0;
            AffineTransform3D transform = identityAffineTransform3D;
        };
        QHash<quint64, ClipboardInstanceReference>
            instanceCollectionReferences;
        const QJsonValue instanceReferencesValue = payload.value(
            QStringLiteral("instance_collection_references"));
        if (!instanceReferencesValue.isArray() ||
            instanceReferencesValue.toArray().size() >
                clipboardDocument.objects().size()) {
            return;
        }
        for (const QJsonValue &referenceValue :
             instanceReferencesValue.toArray()) {
            if (!referenceValue.isObject()) return;
            const QJsonObject reference = referenceValue.toObject();
            bool objectIdValid = false;
            bool collectionIdValid = false;
            const quint64 objectId = reference.value(
                QStringLiteral("object_id")).toString()
                .toULongLong(&objectIdValid);
            const quint64 collectionId = reference.value(
                QStringLiteral("collection_id")).toString()
                .toULongLong(&collectionIdValid);
            AffineTransform3D instanceTransform =
                identityAffineTransform3D;
            const QJsonValue transformValue = reference.value(
                QStringLiteral("instance_transform"));
            if (!transformValue.isUndefined()) {
                if (!transformValue.isArray() ||
                    transformValue.toArray().size() != 16) {
                    return;
                }
                const QJsonArray transformArray = transformValue.toArray();
                for (int index = 0; index < 16; ++index) {
                    const QJsonValue component = transformArray[index];
                    if (!component.isDouble() ||
                        !std::isfinite(component.toDouble())) {
                        return;
                    }
                    instanceTransform[static_cast<std::size_t>(index)] =
                        component.toDouble();
                }
                if (!isValidLinearTransform(instanceTransform)) return;
            }
            if (!objectIdValid || objectId == 0 || !collectionIdValid ||
                collectionId == 0 ||
                instanceCollectionReferences.contains(objectId) ||
                std::none_of(clipboardDocument.objects().cbegin(),
                             clipboardDocument.objects().cend(),
                             [objectId](const SceneObject &object) {
                    return object.id.value() == objectId;
                })) {
                return;
            }
            instanceCollectionReferences.insert(
                objectId, {collectionId, instanceTransform});
        }
        QVector<SceneObject> restoredObjects = clipboardDocument.objects();
        for (SceneObject &object : restoredObjects) {
            const auto reference = instanceCollectionReferences.constFind(
                object.id.value());
            if (reference != instanceCollectionReferences.cend()) {
                object.instanceCollectionId =
                    CollectionId::fromValue(reference->collectionId);
                object.instanceTransform = reference->transform;
            }
        }

        if (collectionClipboard) {
            QVector<CollectionId> rootIds;
            const QJsonValue rootIdsValue = payload.value(
                QStringLiteral("root_collection_ids"));
            if (rootIdsValue.isArray()) {
                const QJsonArray rootIdsArray = rootIdsValue.toArray();
                if (rootIdsArray.isEmpty() || rootIdsArray.size() > 100000) {
                    return;
                }
                QSet<quint64> uniqueRootIds;
                for (const QJsonValue &rootIdValue : rootIdsArray) {
                    bool rootIdValid = false;
                    const quint64 rootId = rootIdValue.toString()
                        .toULongLong(&rootIdValid);
                    if (!rootIdValid || rootId == 0 ||
                        uniqueRootIds.contains(rootId) ||
                        clipboardDocument.collection(
                            CollectionId::fromValue(rootId)) == nullptr) {
                        return;
                    }
                    uniqueRootIds.insert(rootId);
                    rootIds.append(CollectionId::fromValue(rootId));
                }
            } else {
                bool rootIdValid = false;
                const quint64 rootId = payload.value(
                    QStringLiteral("root_collection_id")).toString()
                    .toULongLong(&rootIdValid);
                if (!rootIdValid || rootId == 0 ||
                    clipboardDocument.collection(
                        CollectionId::fromValue(rootId)) == nullptr) {
                    return;
                }
                rootIds.append(CollectionId::fromValue(rootId));
            }
            if (rootIds.isEmpty()) {
                return;
            }
            outlinerCollectionClipboardRootIds_ = std::move(rootIds);
            outlinerCollectionClipboardCollections_ =
                clipboardDocument.collections();
            outlinerCollectionClipboardObjects_ = std::move(restoredObjects);
            outlinerCollectionClipboardMaterials_ =
                clipboardDocument.materials();
            outlinerCollectionClipboardAnchorIdentities_ =
                std::move(anchorIdentities);
            outlinerCollectionClipboardCollectionIdentities_ =
                std::move(collectionIdentities);
        } else {
            outlinerObjectClipboard_ = std::move(restoredObjects);
            outlinerObjectClipboardMaterials_ = clipboardDocument.materials();
            outlinerObjectClipboardAnchorIdentities_ =
                std::move(anchorIdentities);
            outlinerObjectClipboardCollectionIdentities_ =
                std::move(collectionIdentities);
        }
    }

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

        const QFileInfo selectedFileInfo(path);
        QByteArray sourceSha256BeforeImport;
        if (!selectedFileInfo.isSymLink() &&
            selectedFileInfo.suffix().compare(QStringLiteral("blend"),
                                              Qt::CaseInsensitive) == 0) {
            sourceSha256BeforeImport = blenderProjectSha256(path);
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
        importedBlenderSourcePath_.clear();
        importedBlenderSourceSha256_.clear();
        if (importedOrdinaryBlend && !sourceSha256BeforeImport.isEmpty() &&
            !selectedFileInfo.isSymLink()) {
            const QByteArray sourceSha256AfterImport =
                blenderProjectSha256(path);
            if (sourceSha256AfterImport == sourceSha256BeforeImport) {
                importedBlenderSourcePath_ = selectedFileInfo.absoluteFilePath();
                importedBlenderSourceSha256_ = sourceSha256AfterImport;
            } else {
                statusBar()->showMessage(
                    QStringLiteral("The Blender file changed while it was importing; Orphan Data Purge is disabled."),
                    8000);
            }
        }
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

    void purgeImportedBlenderOrphans()
    {
        if (importedBlenderSourcePath_.isEmpty() ||
            importedBlenderSourceSha256_.isEmpty()) {
            return;
        }
        const QString sourcePath = importedBlenderSourcePath_;
        const QString confirmation = QStringLiteral(
            "Remove all unused data blocks from this original Blender file?\n\n"
            "%1\n\n"
            "This changes the Blender file. classiCAD will create and verify a timestamped backup beside it before replacing it.")
            .arg(sourcePath);
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            QStringLiteral("Purge Blender Orphan Data"),
            confirmation,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;

        BlenderOrphanPurgeResult purgeResult;
        QString errorMessage;
        if (!purgeBlenderOrphanData(sourcePath,
                                   importedBlenderSourceSha256_,
                                   &purgeResult,
                                   &errorMessage)) {
            QMessageBox::critical(
                this,
                QStringLiteral("Could Not Purge Blender Orphan Data"),
                errorMessage.isEmpty()
                    ? QStringLiteral("Blender could not purge orphan data.")
                    : errorMessage);
            return;
        }

        importedBlenderSourceSha256_ = purgeResult.sourceSha256;
        DocumentSettings settings = session_.document().settings();
        settings.importedBlenderOrphans = purgeResult.remainingOrphans;
        bool settingsCommitted = false;
        {
            DocumentTransaction transaction = session_.beginTransaction();
            settingsCommitted = transaction.setSettings(settings) &&
                                session_.commitTransaction(transaction);
        }
        if (!settingsCommitted) {
            if (!session_.document().setSettings(settings)) {
                importedBlenderSourcePath_.clear();
                importedBlenderSourceSha256_.clear();
                QMessageBox::critical(
                    this,
                    QStringLiteral("Purge Finished, Refresh Failed"),
                    QStringLiteral("Blender purged the original file and saved a backup at %1, but classiCAD could not refresh its Orphan Data list.")
                        .arg(purgeResult.backupPath));
                return;
            }
            refreshCollections();
            markDocumentModified();
        }
        statusBar()->showMessage(
            QStringLiteral("Removed %1 orphan data block(s). Backup: %2")
                .arg(purgeResult.removedDataBlockCount)
                .arg(purgeResult.backupPath),
            12000);
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
            !importedMaterials.isEmpty() || !report.scenes.isEmpty() ||
            !report.outlinerOnlyObjects.isEmpty()) {
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
            QHash<QString, int> outlinerOnlyIndices;
            for (int index = 0;
                 index < settings.importedBlenderOutlinerOnlyObjects.size();
                 ++index) {
                const QString sourceId = settings
                    .importedBlenderOutlinerOnlyObjects[index].toObject()
                    .value(QStringLiteral("object_id")).toString();
                if (!sourceId.isEmpty()) {
                    outlinerOnlyIndices.insert(sourceId, index);
                }
            }
            for (const QJsonValue &recordValue : report.outlinerOnlyObjects) {
                const QJsonObject record = recordValue.toObject();
                const QString sourceId = record.value(
                    QStringLiteral("object_id")).toString();
                if (sourceId.isEmpty()) continue;
                const auto existing = outlinerOnlyIndices.constFind(sourceId);
                if (existing != outlinerOnlyIndices.cend()) {
                    settings.importedBlenderOutlinerOnlyObjects.replace(
                        existing.value(), record);
                } else if (settings.importedBlenderOutlinerOnlyObjects.size() <
                           100000) {
                    outlinerOnlyIndices.insert(
                        sourceId,
                        settings.importedBlenderOutlinerOnlyObjects.size());
                    settings.importedBlenderOutlinerOnlyObjects.append(record);
                }
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
            for (auto it = report.activeCollectionPathsByViewLayer.begin();
                 it != report.activeCollectionPathsByViewLayer.end(); ++it) {
                settings.activeCollectionPathsByViewLayer.insert(
                    it.key(), it.value());
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
        importedBlenderSourcePath_.clear();
        importedBlenderSourceSha256_.clear();
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
                if (collectionsPanel_ != nullptr) {
                    collectionsPanel_->syncSelection();
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
                return sceneName.isEmpty()
                    ? !collection.sceneMembershipExplicit
                    : (collection.sceneMembershipExplicit
                           ? collection.sourceSceneNames.contains(sceneName)
                           : (collection.sourceSceneNames.isEmpty() ||
                              collection.sourceSceneNames.contains(sceneName)));
            };
        const auto collectionBelongsToScene = [](
            const DocumentCollection &collection, const QString &sceneName) {
            return collection.sceneMembershipExplicit
                ? collection.sourceSceneNames.contains(sceneName)
                : (collection.sourceSceneNames.isEmpty() ||
                   collection.sourceSceneNames.contains(sceneName));
        };
        const auto updateSceneCollectionIds = [this](
            DocumentTransaction &transaction,
            const QString &sceneId,
            const QStringList &sourceCollectionIds) {
            if (sceneId.isEmpty()) return false;
            DocumentSettings settings = session_.document().settings();
            QJsonArray scenes = settings.importedBlenderScenes;
            bool found = false;
            bool changed = false;
            for (qsizetype index = 0; index < scenes.size(); ++index) {
                QJsonObject scene = scenes[index].toObject();
                if (scene.value(QStringLiteral("scene_id")).toString() !=
                    sceneId) {
                    continue;
                }
                found = true;
                QJsonArray ids;
                for (const QString &sourceId : sourceCollectionIds) {
                    ids.append(sourceId);
                }
                if (scene.value(QStringLiteral("collection_ids")).toArray() !=
                    ids) {
                    scene.insert(QStringLiteral("collection_ids"), ids);
                    scenes[index] = scene;
                    changed = true;
                }
                break;
            }
            if (!found) return false;
            if (!changed) return true;
            settings.importedBlenderScenes = scenes;
            return transaction.setSettings(settings);
        };
        const auto updateAllSceneCollectionIds =
            [this, activeSceneRecord](DocumentTransaction &transaction) {
                DocumentSettings settings = session_.document().settings();
                QJsonArray scenes = settings.importedBlenderScenes;
                const QString activeSceneId = activeSceneRecord().value(
                    QStringLiteral("scene_id")).toString();
                bool changed = false;
                for (qsizetype sceneIndex = 0; sceneIndex < scenes.size();
                     ++sceneIndex) {
                    QJsonObject scene = scenes[sceneIndex].toObject();
                    const QString sceneId = scene.value(
                        QStringLiteral("scene_id")).toString();
                    const QString sceneName = scene.value(
                        QStringLiteral("name")).toString();
                    if (sceneId.isEmpty() || sceneName.isEmpty()) continue;
                    QJsonArray collectionIds;
                    for (const DocumentCollection &collection :
                         session_.document().collections()) {
                        const bool belongs =
                            collection.sceneMembershipExplicit
                                ? collection.sourceSceneNames.contains(sceneName)
                                : (collection.sourceSceneNames.isEmpty()
                                       ? sceneId == activeSceneId
                                       : collection.sourceSceneNames.contains(
                                             sceneName));
                        if (belongs && !collection.sourceId.isEmpty()) {
                            collectionIds.append(collection.sourceId);
                        }
                    }
                    if (scene.value(QStringLiteral("collection_ids")).toArray() ==
                        collectionIds) {
                        continue;
                    }
                    scene.insert(QStringLiteral("collection_ids"),
                                 collectionIds);
                    scenes[sceneIndex] = scene;
                    changed = true;
                }
                if (!changed) return true;
                settings.importedBlenderScenes = scenes;
                return transaction.setSettings(settings);
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
            QHash<QString, CollectionId> collectionIdBySourceId;
            for (const DocumentCollection &collection : document.collections()) {
                if (!collection.sourceId.isEmpty()) {
                    collectionIdBySourceId.insert(collection.sourceId,
                                                  collection.id);
                }
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
            const auto jsonStringList = [](const QJsonValue &value) {
                QStringList names;
                for (const QJsonValue &nameValue : value.toArray()) {
                    if (nameValue.isString()) names.append(nameValue.toString());
                }
                return names;
            };
            const auto toOutlinerAnimationFromJson =
                [&jsonStringList](const QJsonObject &source) {
                    OutlinerAnimationEntry animation;
                    animation.hasAnimationData = source.value(
                        QStringLiteral("has_animation_data")).toBool();
                    animation.actionName = source.value(
                        QStringLiteral("action_name")).toString();
                    animation.actionSlotName = source.value(
                        QStringLiteral("action_slot_name")).toString();
                    animation.hasDrivers = source.value(
                        QStringLiteral("has_drivers")).toBool();
                    animation.driverTargetNames = jsonStringList(source.value(
                        QStringLiteral("driver_target_names")));
                    for (const QJsonValue &trackValue : source.value(
                             QStringLiteral("nla_tracks")).toArray()) {
                        const QJsonObject track = trackValue.toObject();
                        animation.nlaTracks.append({
                            track.value(QStringLiteral("name")).toString(),
                            jsonStringList(track.value(
                                QStringLiteral("action_names")))});
                    }
                    return animation;
                };
            for (const SceneObject &object : document.objects()) {
                if (object.blenderLooseMeshEdgeCompanion) continue;
                QString name = object.displayName.trimmed();
                if (name.isEmpty()) {
                    name = QStringLiteral("Object %1").arg(object.id.value());
                }
                OutlinerObjectEntry entry;
                entry.id = object.id;
                entry.hierarchyIdentity = object.blenderSourceObjectId.isEmpty()
                    ? QStringLiteral("classiCAD-local-object:%1")
                          .arg(object.id.value())
                    : object.blenderSourceObjectId;
                entry.parentHierarchyIdentity = object.blenderParentObjectId;
                entry.name = name;
                entry.blenderType = object.blenderSourceObjectType;
                entry.assetMarked = object.assetMarked;
                entry.libraryPath = object.libraryPath;
                if (object.instanceCollectionId.isValid()) {
                    const DocumentCollection *instanceCollection =
                        document.collection(object.instanceCollectionId);
                    if (instanceCollection != nullptr) {
                        entry.instancedCollectionName =
                            instanceCollection->name;
                    }
                }
                entry.dataBlockName = object.blenderSourceDataName;
                entry.greasePencilLayerTree =
                    object.blenderSourceGreasePencilLayerTree;
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
                for (const BlenderSourceShaderEffect &effect :
                     object.blenderSourceShaderEffects) {
                    entry.shaderEffects.append({effect.name, effect.linkedName,
                                                effect.linkedType});
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
                QStringList materialIdsForRows;
                if (object.geometry.geometryType == GeometryType::PolygonMesh &&
                    !object.geometry.polygonMesh.materialSlotIds.isEmpty()) {
                    const PolygonMesh3D &mesh = object.geometry.polygonMesh;
                    entry.materialNames.clear();
                    int sourceMaterialIndex = 0;
                    for (const QString &materialId : mesh.materialSlotIds) {
                        if (materialId.isEmpty()) continue;
                        QString materialName = materialNamesById.value(materialId);
                        if (materialName.isEmpty() &&
                            sourceMaterialIndex <
                                object.blenderSourceMaterialNames.size()) {
                            materialName = object.blenderSourceMaterialNames[
                                sourceMaterialIndex];
                        }
                        ++sourceMaterialIndex;
                        if (materialName.isEmpty()) continue;
                        entry.materialNames.append(materialName);
                        materialIdsForRows.append(materialId);
                    }
                } else if (entry.materialNames.isEmpty() &&
                           object.geometry.geometryType ==
                               GeometryType::PolygonMesh) {
                    for (const PolygonMeshMaterialSlot &material :
                         object.geometry.polygonMesh.materialSlots) {
                        if (!material.name.isEmpty()) {
                            entry.materialNames.append(material.name);
                        }
                    }
                }
                for (qsizetype index = 0;
                     index < entry.materialNames.size(); ++index) {
                    OutlinerMaterialEntry material;
                    material.name = entry.materialNames[index];
                    if (index < materialIdsForRows.size()) {
                        material.materialId = materialIdsForRows[index];
                    }
                    if (index < object.blenderSourceMaterialAnimations.size()) {
                        material.animation = toOutlinerAnimation(
                            object.blenderSourceMaterialAnimations[index]);
                    }
                    entry.materialEntries.append(std::move(material));
                }
                entry.collectionIds = object.collectionIds;
                entry.hideViewport = object.hideViewport;
                entry.hiddenInViewport = object.hiddenInViewport;
                entry.hideRender = object.hideRender;
                entry.hideSelect = object.hideSelect;
                entry.visible = document.isObjectVisible(object.id);
                entry.selectable = document.isObjectEditable(object.id);
                entry.viewLayerHidden = object.viewLayerHidden;
                objects.append(std::move(entry));
            }
            for (const QJsonValue &recordValue :
                 document.settings().importedBlenderOutlinerOnlyObjects) {
                if (!recordValue.isObject()) continue;
                const QJsonObject record = recordValue.toObject();
                const QString sourceId = record.value(
                    QStringLiteral("object_id")).toString();
                const QString objectType = record.value(
                    QStringLiteral("object_type")).toString();
                if (sourceId.isEmpty() || objectType.isEmpty() ||
                    objectIdsByIdentity.contains(sourceId)) continue;

                OutlinerObjectEntry entry;
                entry.outlinerOnly = true;
                entry.sourceObjectId = sourceId;
                entry.hierarchyIdentity = sourceId;
                entry.parentHierarchyIdentity = record.value(
                    QStringLiteral("parent_id")).toString();
                entry.outlinerDescription = record.value(
                    QStringLiteral("reason")).toString();
                entry.instancedCollectionName = record.value(
                    QStringLiteral("instance_collection_name")).toString();
                if (entry.instancedCollectionName.isEmpty()) {
                    entry.instancedCollectionName = record.value(
                        QStringLiteral("instance_collection")).toString();
                }
                entry.hasPose = record.value(
                    QStringLiteral("has_pose")).toBool();
                entry.showPoseChannels = record.value(
                    QStringLiteral("show_pose_channels")).toBool();
                entry.poseBoneTree = record.value(
                    QStringLiteral("pose_bone_tree")).toArray();
                entry.armatureBoneTree = record.value(
                    QStringLiteral("armature_bone_tree")).toArray();
                entry.boneCollectionTree = record.value(
                    QStringLiteral("bone_collection_tree")).toArray();
                entry.name = record.value(QStringLiteral("name")).toString();
                entry.blenderType = objectType;
                entry.dataBlockName = record.value(
                    QStringLiteral("data_name")).toString();
                entry.greasePencilLayerTree = record.value(
                    QStringLiteral("grease_pencil_layer_tree")).toArray();
                entry.shapeKeyDataName = record.value(
                    QStringLiteral("shape_key_data_name")).toString();
                entry.shapeKeyNames = jsonStringList(record.value(
                    QStringLiteral("shape_key_names")));
                entry.dataMaterialNames = jsonStringList(record.value(
                    QStringLiteral("data_material_names")));
                const QJsonArray dataMaterialAnimations = record.value(
                    QStringLiteral("data_material_animations")).toArray();
                for (qsizetype index = 0;
                     index < entry.dataMaterialNames.size(); ++index) {
                    OutlinerMaterialEntry material;
                    material.name = entry.dataMaterialNames[index];
                    if (index < dataMaterialAnimations.size()) {
                        material.animation = toOutlinerAnimationFromJson(
                            dataMaterialAnimations[index].toObject());
                    }
                    entry.dataMaterialEntries.append(std::move(material));
                }
                const OutlinerAnimationEntry dataAnimation =
                    toOutlinerAnimationFromJson(record.value(
                        QStringLiteral("data_animation")).toObject());
                entry.dataHasAnimationData = dataAnimation.hasAnimationData;
                entry.dataAnimationActionName = dataAnimation.actionName;
                entry.dataAnimationActionSlotName = dataAnimation.actionSlotName;
                entry.dataHasDrivers = dataAnimation.hasDrivers;
                entry.dataDriverTargetNames = dataAnimation.driverTargetNames;
                entry.dataNlaTracks = dataAnimation.nlaTracks;

                entry.materialNames = jsonStringList(record.value(
                    QStringLiteral("material_slot_names")));
                const QJsonArray materialAnimations = record.value(
                    QStringLiteral("material_slot_animations")).toArray();
                for (qsizetype index = 0; index < entry.materialNames.size();
                     ++index) {
                    OutlinerMaterialEntry material;
                    material.name = entry.materialNames[index];
                    if (index < materialAnimations.size()) {
                        material.animation = toOutlinerAnimationFromJson(
                            materialAnimations[index].toObject());
                    }
                    entry.materialEntries.append(std::move(material));
                }
                entry.modifierNames = jsonStringList(record.value(
                    QStringLiteral("modifier_names")));
                for (const QJsonValue &modifierValue : record.value(
                         QStringLiteral("modifier_entries")).toArray()) {
                    const QJsonObject modifier = modifierValue.toObject();
                    entry.modifiers.append({
                        modifier.value(QStringLiteral("name")).toString(),
                        modifier.value(QStringLiteral("linked_name")).toString(),
                        modifier.value(QStringLiteral("linked_type")).toString()});
                }
                for (const QJsonValue &effectValue : record.value(
                         QStringLiteral("shader_effect_entries")).toArray()) {
                    const QJsonObject effect = effectValue.toObject();
                    entry.shaderEffects.append({
                        effect.value(QStringLiteral("name")).toString(),
                        effect.value(QStringLiteral("linked_name")).toString(),
                        effect.value(QStringLiteral("linked_type")).toString()});
                }
                entry.constraintNames = jsonStringList(record.value(
                    QStringLiteral("constraint_names")));
                entry.vertexGroupNames = jsonStringList(record.value(
                    QStringLiteral("vertex_group_names")));
                const OutlinerAnimationEntry animation =
                    toOutlinerAnimationFromJson(record);
                entry.hasAnimationData = animation.hasAnimationData;
                entry.animationActionName = animation.actionName;
                entry.animationActionSlotName = animation.actionSlotName;
                entry.hasDrivers = animation.hasDrivers;
                entry.driverTargetNames = animation.driverTargetNames;
                entry.nlaTracks = animation.nlaTracks;

                for (const QJsonValue &sourceCollectionId : record.value(
                         QStringLiteral("collection_ids")).toArray()) {
                    const CollectionId collectionId =
                        collectionIdBySourceId.value(
                            sourceCollectionId.toString(),
                            CollectionId::invalid());
                    if (collectionId.isValid() &&
                        !entry.collectionIds.contains(collectionId)) {
                        entry.collectionIds.append(collectionId);
                    }
                }
                entry.sceneKeys = sceneKeysBySourceObjectId.value(sourceId);
                const QJsonObject visibility = record.value(
                    QStringLiteral("visibility")).toObject();
                entry.hideViewport = visibility.value(
                    QStringLiteral("hide_viewport")).toBool();
                entry.hideRender = visibility.value(
                    QStringLiteral("hide_render")).toBool();
                entry.hideSelect = visibility.value(
                    QStringLiteral("hide_select")).toBool();
                entry.visible = !entry.hideViewport &&
                    !visibility.value(QStringLiteral("hidden_in_viewport")).toBool();
                entry.selectable = false;
                entry.viewLayerHidden = record.value(
                    QStringLiteral("view_layer_hidden")).toObject();
                objects.append(std::move(entry));
            }
            return objects;
        };
        collectionCallbacks.scenes = [this]() {
            QVector<OutlinerSceneEntry> scenes;
            const auto readAnimationData = [](
                const QJsonObject &source) {
                OutlinerAnimationEntry result;
                result.hasAnimationData = source.value(
                    QStringLiteral("has_animation_data")).toBool();
                result.actionName = source.value(
                    QStringLiteral("action_name")).toString();
                result.actionSlotName = source.value(
                    QStringLiteral("action_slot_name")).toString();
                result.hasDrivers = source.value(
                    QStringLiteral("has_drivers")).toBool();
                for (const QJsonValue &targetValue : source.value(
                         QStringLiteral("driver_target_names")).toArray()) {
                    const QString targetName = targetValue.toString();
                    if (!targetName.isEmpty()) {
                        result.driverTargetNames.append(targetName);
                    }
                }
                for (const QJsonValue &trackValue : source.value(
                         QStringLiteral("nla_tracks")).toArray()) {
                    const QJsonObject sourceTrack = trackValue.toObject();
                    OutlinerNlaTrackEntry track;
                    track.name = sourceTrack.value(
                        QStringLiteral("name")).toString();
                    for (const QJsonValue &actionValue : sourceTrack.value(
                             QStringLiteral("action_names")).toArray()) {
                        const QString actionName = actionValue.toString();
                        if (!actionName.isEmpty()) {
                            track.actionNames.append(actionName);
                        }
                    }
                    result.nlaTracks.append(std::move(track));
                }
                return result;
            };
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
                    entry.worldName = scene.value(
                        QStringLiteral("world_name")).toString();
                    entry.worldAnimation = readAnimationData(scene.value(
                        QStringLiteral("world_animation_data")).toObject());
                    entry.animationData = readAnimationData(scene.value(
                        QStringLiteral("scene_animation_data")).toObject());
                    for (const QJsonValue &layerValue : scene.value(
                             QStringLiteral("view_layers")).toArray()) {
                        const QJsonObject layer = layerValue.toObject();
                        const QString layerKey = layer.value(
                            QStringLiteral("key")).toString();
                        const QString layerName = layer.value(
                            QStringLiteral("name")).toString();
                        if (layerKey.isEmpty() || layerName.isEmpty()) continue;
                        OutlinerViewLayerEntry viewLayerEntry;
                        viewLayerEntry.key = layerKey;
                        viewLayerEntry.displayName = layerName;
                        entry.viewLayers.append(std::move(viewLayerEntry));
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
        collectionCallbacks.libraries = [this]() {
            QVector<OutlinerLibraryEntry> libraries;
            const auto readAnimationData = [](
                const QJsonObject &source) {
                OutlinerAnimationEntry result;
                result.hasAnimationData = source.value(
                    QStringLiteral("has_animation_data")).toBool();
                result.actionName = source.value(
                    QStringLiteral("action_name")).toString();
                result.actionSlotName = source.value(
                    QStringLiteral("action_slot_name")).toString();
                result.hasDrivers = source.value(
                    QStringLiteral("has_drivers")).toBool();
                for (const QJsonValue &targetValue : source.value(
                         QStringLiteral("driver_target_names")).toArray()) {
                    const QString targetName = targetValue.toString();
                    if (!targetName.isEmpty()) {
                        result.driverTargetNames.append(targetName);
                    }
                }
                for (const QJsonValue &trackValue : source.value(
                         QStringLiteral("nla_tracks")).toArray()) {
                    const QJsonObject sourceTrack = trackValue.toObject();
                    OutlinerNlaTrackEntry track;
                    track.name = sourceTrack.value(
                        QStringLiteral("name")).toString();
                    for (const QJsonValue &actionValue : sourceTrack.value(
                             QStringLiteral("action_names")).toArray()) {
                        const QString actionName = actionValue.toString();
                        if (!actionName.isEmpty()) {
                            track.actionNames.append(actionName);
                        }
                    }
                    result.nlaTracks.append(std::move(track));
                }
                return result;
            };
            for (const QJsonValue &libraryValue :
                 session_.document().settings().importedBlenderLibraries) {
                const QJsonObject record = libraryValue.toObject();
                OutlinerLibraryEntry entry;
                entry.id = record.value(QStringLiteral("id")).toString();
                entry.displayName = record.value(
                    QStringLiteral("name")).toString();
                entry.parentId = record.value(
                    QStringLiteral("parent_library_id")).toString();
                entry.currentFile = record.value(
                    QStringLiteral("is_current_file")).toBool();
                entry.truncated = record.value(
                    QStringLiteral("truncated")).toBool();
                for (const QJsonValue &groupValue : record.value(
                         QStringLiteral("data_blocks")).toArray()) {
                    const QJsonObject groupRecord = groupValue.toObject();
                    OutlinerLibraryDataGroup group;
                    group.displayName = groupRecord.value(
                        QStringLiteral("name")).toString();
                    for (const QJsonValue &nameValue : groupRecord.value(
                             QStringLiteral("names")).toArray()) {
                        const QString name = nameValue.toString();
                        if (!name.isEmpty()) group.dataBlockNames.append(name);
                    }
                    for (const QJsonValue &animationValue : groupRecord.value(
                             QStringLiteral("animations")).toArray()) {
                        const QJsonObject animationRecord =
                            animationValue.toObject();
                        const QString dataBlockName = animationRecord.value(
                            QStringLiteral("data_block_name")).toString();
                        const OutlinerAnimationEntry animation = readAnimationData(
                            animationRecord.value(
                                QStringLiteral("animation")).toObject());
                        if (!dataBlockName.isEmpty() &&
                            animation.hasAnimationData) {
                            group.animationsByDataBlockName.insert(
                                dataBlockName, animation);
                        }
                    }
                    for (const QJsonValue &subtreeValue : groupRecord.value(
                             QStringLiteral("data_block_subtrees")).toArray()) {
                        const QJsonObject subtree = subtreeValue.toObject();
                        const QString dataBlockName = subtree.value(
                            QStringLiteral("data_block_name")).toString();
                        if (dataBlockName.isEmpty()) continue;
                        OutlinerLibraryDataGroup::DataBlockTree dataBlockTree;
                        dataBlockTree.shapeKeyDataName = subtree.value(
                            QStringLiteral("shape_key_data_name")).toString();
                        for (const QJsonValue &keyName : subtree.value(
                                 QStringLiteral("shape_key_names")).toArray()) {
                            const QString name = keyName.toString();
                            if (!name.isEmpty()) {
                                dataBlockTree.shapeKeyNames.append(name);
                            }
                        }
                        for (const QJsonValue &materialName : subtree.value(
                                 QStringLiteral("material_names")).toArray()) {
                            const QString name = materialName.toString();
                            if (!name.isEmpty()) {
                                dataBlockTree.materialNames.append(name);
                            }
                        }
                        dataBlockTree.armatureBoneTree = subtree.value(
                            QStringLiteral("armature_bone_tree")).toArray();
                        dataBlockTree.boneCollectionTree = subtree.value(
                            QStringLiteral("bone_collection_tree")).toArray();
                        dataBlockTree.greasePencilLayerTree = subtree.value(
                            QStringLiteral("grease_pencil_layer_tree")).toArray();
                        dataBlockTree.collectionTree = subtree.value(
                            QStringLiteral("collection_tree")).toArray();
                        dataBlockTree.objectTree = subtree.value(
                            QStringLiteral("object_tree")).toArray();
                        dataBlockTree.sceneTree = subtree.value(
                            QStringLiteral("scene_tree")).toObject();
                        dataBlockTree.textureImageName = subtree.value(
                            QStringLiteral("texture_image_name")).toString();
                        for (const QJsonValue &textureName : subtree.value(
                                 QStringLiteral("line_style_texture_names")).toArray()) {
                            const QString name = textureName.toString();
                            if (!name.isEmpty()) {
                                dataBlockTree.lineStyleTextureNames.append(name);
                            }
                        }
                        group.dataBlockTreesByName.insert(
                            dataBlockName, std::move(dataBlockTree));
                    }
                    for (const QJsonValue &actionValue : groupRecord.value(
                             QStringLiteral("action_slots")).toArray()) {
                        const QJsonObject actionRecord = actionValue.toObject();
                        const QString actionName = actionRecord.value(
                            QStringLiteral("action_name")).toString();
                        if (actionName.isEmpty()) continue;
                        QVector<OutlinerActionSlotEntry> slotEntries;
                        for (const QJsonValue &slotValue : actionRecord.value(
                                 QStringLiteral("slots")).toArray()) {
                            const QJsonObject slotRecord = slotValue.toObject();
                            OutlinerActionSlotEntry slot;
                            slot.displayName = slotRecord.value(
                                QStringLiteral("name")).toString();
                            slot.identifier = slotRecord.value(
                                QStringLiteral("identifier")).toString();
                            if (!slot.displayName.isEmpty() &&
                                !slot.identifier.isEmpty()) {
                                slotEntries.append(std::move(slot));
                            }
                        }
                        group.actionSlotsByDataBlockName.insert(
                            actionName, std::move(slotEntries));
                    }
                    entry.dataGroups.append(std::move(group));
                }
                if (!entry.id.isEmpty() && !entry.displayName.isEmpty()) {
                    libraries.append(std::move(entry));
                }
            }
            return libraries;
        };
        collectionCallbacks.orphanGroups = [this]() {
            QVector<OutlinerOrphanGroup> groups;
            for (const QJsonValue &groupValue :
                 session_.document().settings().importedBlenderOrphans) {
                const QJsonObject record = groupValue.toObject();
                OutlinerOrphanGroup group;
                group.displayName = record.value(
                    QStringLiteral("name")).toString();
                group.truncated = record.value(
                    QStringLiteral("truncated")).toBool();
                for (const QJsonValue &nameValue : record.value(
                         QStringLiteral("names")).toArray()) {
                    const QString name = nameValue.toString();
                    if (!name.isEmpty()) group.dataBlockNames.append(name);
                }
                if (!group.displayName.isEmpty()) {
                    groups.append(std::move(group));
                }
            }
            return groups;
        };
        collectionCallbacks.canPurgeOrphanData = [this]() {
            return !importedBlenderSourcePath_.isEmpty() &&
                   importedBlenderSourceSha256_.size() == 64 &&
                   !session_.document().settings().importedBlenderOrphans.isEmpty();
        };
        collectionCallbacks.purgeOrphanData = [this]() {
            purgeImportedBlenderOrphans();
        };
        collectionCallbacks.sequencer = [this]() {
            OutlinerSequenceTree tree;
            const QJsonObject record = session_.document().settings()
                                           .importedBlenderSequencer;
            tree.workspaceName = record.value(
                QStringLiteral("workspace_name")).toString();
            tree.sceneName = record.value(
                QStringLiteral("scene_name")).toString();
            tree.truncated = record.value(
                QStringLiteral("truncated")).toBool();
            std::function<OutlinerSequenceStrip(const QJsonObject &)> parseStrip;
            parseStrip = [&parseStrip](const QJsonObject &source) {
                OutlinerSequenceStrip strip;
                strip.sourceId = source.value(
                    QStringLiteral("source_id")).toString();
                strip.kind = source.value(QStringLiteral("kind")).toString();
                strip.name = source.value(QStringLiteral("name")).toString();
                strip.type = source.value(QStringLiteral("type")).toString();
                strip.mediaFilename = source.value(
                    QStringLiteral("media_filename")).toString();
                strip.dataDirectory = source.value(
                    QStringLiteral("data_directory")).toString();
                strip.isMeta = source.value(
                    QStringLiteral("is_meta")).toBool();
                strip.isEffect = source.value(
                    QStringLiteral("is_effect")).toBool();
                strip.selected = source.value(
                    QStringLiteral("selected")).toBool();
                strip.active = source.value(
                    QStringLiteral("active")).toBool();
                for (const QJsonValue &childValue : source.value(
                         QStringLiteral("children")).toArray()) {
                    if (childValue.isObject()) {
                        strip.children.append(parseStrip(childValue.toObject()));
                    }
                }
                return strip;
            };
            for (const QJsonValue &stripValue : record.value(
                     QStringLiteral("strips")).toArray()) {
                if (stripValue.isObject()) {
                    tree.strips.append(parseStrip(stripValue.toObject()));
                }
            }
            return tree;
        };
        collectionCallbacks.dataApi = [this]() {
            return session_.document().settings().importedBlenderDataApi;
        };
        collectionCallbacks.setSequencerSelection = [this](
            const QStringList &selectedStripIds, const QString &activeStripId) {
            DocumentSettings settings = session_.document().settings();
            QJsonObject sequencer = settings.importedBlenderSequencer;
            QSet<QString> selectedIds;
            for (const QString &sourceId : selectedStripIds) {
                selectedIds.insert(sourceId);
            }
            bool changed = false;
            const auto updateRows = [&](const auto &self,
                                        const QJsonArray &rows) -> QJsonArray {
                QJsonArray updated;
                for (const QJsonValue &rowValue : rows) {
                    if (!rowValue.isObject()) {
                        updated.append(rowValue);
                        continue;
                    }
                    QJsonObject row = rowValue.toObject();
                    if (row.value(QStringLiteral("kind")).toString() ==
                        QLatin1String("STRIP")) {
                        const QString sourceId = row.value(
                            QStringLiteral("source_id")).toString();
                        if (!sourceId.isEmpty()) {
                            const bool selected = selectedIds.contains(sourceId);
                            const bool active = sourceId == activeStripId;
                            if (row.value(QStringLiteral("selected")).toBool() !=
                                    selected ||
                                row.value(QStringLiteral("active")).toBool() !=
                                    active) {
                                row.insert(QStringLiteral("selected"), selected);
                                row.insert(QStringLiteral("active"), active);
                                changed = true;
                            }
                        }
                    }
                    const QJsonArray children = row.value(
                        QStringLiteral("children")).toArray();
                    const QJsonArray updatedChildren = self(self, children);
                    if (updatedChildren != children) {
                        row.insert(QStringLiteral("children"), updatedChildren);
                    }
                    updated.append(row);
                }
                return updated;
            };
            const QJsonArray strips = sequencer.value(
                QStringLiteral("strips")).toArray();
            const QJsonArray updatedStrips = updateRows(updateRows, strips);
            if (!changed) return;
            sequencer.insert(QStringLiteral("strips"), updatedStrips);
            settings.importedBlenderSequencer = sequencer;
            DocumentTransaction transaction = session_.beginTransaction();
            if (!transaction.setSettings(settings) ||
                !session_.commitTransaction(transaction)) {
                statusBar()->showMessage(
                    QStringLiteral("Could not update Sequencer Outliner selection"),
                    4000);
            }
        };
        collectionCallbacks.libraryOverrides = [this]() {
            return session_.document().settings().importedBlenderLibraryOverrides;
        };
        collectionCallbacks.activeSceneRecord = activeSceneRecord;
        collectionCallbacks.canEditKeyingSetPaths = [this]() {
            return !importedBlenderSourcePath_.isEmpty() &&
                   importedBlenderSourceSha256_.size() == 64;
        };
        collectionCallbacks.canEditDataApiKeyframes = [this]() {
            return !importedBlenderSourcePath_.isEmpty() &&
                   importedBlenderSourceSha256_.size() == 64;
        };
        collectionCallbacks.activeSceneKey = [activeSceneRecord]() {
            return activeSceneRecord().value(
                QStringLiteral("scene_id")).toString();
        };
        collectionCallbacks.setActiveKeyingSet =
            [this, activeSceneRecord](const QString &key) {
                DocumentSettings settings = session_.document().settings();
                QJsonArray scenes = settings.importedBlenderScenes;
                const QString sceneId = activeSceneRecord().value(
                    QStringLiteral("scene_id")).toString();
                if (sceneId.isEmpty()) return;
                bool changed = false;
                for (qsizetype index = 0; index < scenes.size(); ++index) {
                    QJsonObject scene = scenes[index].toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneId) {
                        continue;
                    }
                    bool keyExists = key.isEmpty();
                    for (const QJsonValue &keyingSetValue : scene.value(
                             QStringLiteral("keying_sets")).toArray()) {
                        if (keyingSetValue.toObject().value(
                                QStringLiteral("key")).toString() == key) {
                            keyExists = true;
                            break;
                        }
                    }
                    if (!keyExists || scene.value(
                            QStringLiteral("active_keying_set_key")).toString() ==
                            key) {
                        return;
                    }
                    scene.insert(QStringLiteral("active_keying_set_key"), key);
                    scenes[index] = scene;
                    changed = true;
                    break;
                }
                if (!changed) return;
                settings.importedBlenderScenes = scenes;
                DocumentTransaction transaction = session_.beginTransaction();
                if (!transaction.setSettings(settings) ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not change the active Keying Set"),
                        4000);
                }
            };
        collectionCallbacks.editKeyingSetPaths =
            [this, activeSceneRecord](const QString &requestedKey,
                                      const QJsonArray &paths,
                                      bool add) -> QString {
                DocumentSettings settings = session_.document().settings();
                QJsonArray scenes = settings.importedBlenderScenes;
                const QJsonArray originalScenes = scenes;
                const QString sceneId = activeSceneRecord().value(
                    QStringLiteral("scene_id")).toString();
                if (sceneId.isEmpty()) return requestedKey;
                QString activeKey = requestedKey;
                for (qsizetype sceneIndex = 0; sceneIndex < scenes.size();
                     ++sceneIndex) {
                    QJsonObject scene = scenes[sceneIndex].toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneId) {
                        continue;
                    }
                    QJsonArray keyingSets = scene.value(
                        QStringLiteral("keying_sets")).toArray();
                    int keyingSetIndex = -1;
                    for (qsizetype index = 0; index < keyingSets.size(); ++index) {
                        if (keyingSets[index].toObject().value(
                                QStringLiteral("key")).toString() == activeKey) {
                            keyingSetIndex = static_cast<int>(index);
                            break;
                        }
                    }
                    if (keyingSetIndex < 0) {
                        if (keyingSets.size() >= 1024) return activeKey;
                        QSet<QString> existingNames;
                        QSet<QString> existingKeys;
                        for (const QJsonValue &value : keyingSets) {
                            const QJsonObject keyingSet = value.toObject();
                            existingNames.insert(keyingSet.value(
                                QStringLiteral("name")).toString());
                            existingKeys.insert(keyingSet.value(
                                QStringLiteral("key")).toString());
                        }
                        QString name = QStringLiteral("KeyingSet");
                        for (int suffix = 1; existingNames.contains(name); ++suffix) {
                            name = QStringLiteral("KeyingSet.%1")
                                       .arg(suffix, 3, 10, QLatin1Char('0'));
                        }
                        int suffix = 1;
                        do {
                            activeKey = QStringLiteral("classiCAD-keying-set:%1")
                                            .arg(suffix++);
                        } while (existingKeys.contains(activeKey));
                        QJsonObject keyingSet;
                        keyingSet.insert(QStringLiteral("key"), activeKey);
                        keyingSet.insert(QStringLiteral("name"), name);
                        keyingSet.insert(QStringLiteral("is_absolute"), true);
                        keyingSet.insert(QStringLiteral("paths"), QJsonArray{});
                        keyingSets.append(keyingSet);
                        keyingSetIndex = keyingSets.size() - 1;
                    }

                    QJsonObject keyingSet = keyingSets[keyingSetIndex].toObject();
                    const QJsonArray previousPaths = keyingSet.value(
                        QStringLiteral("paths")).toArray();
                    QJsonArray updatedPaths;
                    const auto samePath = [](const QJsonObject &left,
                                             const QJsonObject &right) {
                        static const QStringList keys{
                            QStringLiteral("id_type"),
                            QStringLiteral("id_name"),
                            QStringLiteral("library_path"),
                            QStringLiteral("data_path"),
                            QStringLiteral("array_index")};
                        for (const QString &key : keys) {
                            if (left.value(key) != right.value(key)) return false;
                        }
                        return true;
                    };
                    if (add) {
                        updatedPaths = previousPaths;
                        for (const QJsonValue &candidateValue : paths) {
                            if (!candidateValue.isObject() ||
                                updatedPaths.size() >= 20000) {
                                continue;
                            }
                            const QJsonObject candidate = candidateValue.toObject();
                            const bool exists = std::any_of(
                                updatedPaths.cbegin(), updatedPaths.cend(),
                                [&candidate, &samePath](const QJsonValue &value) {
                                    return value.isObject() &&
                                           samePath(value.toObject(), candidate);
                                });
                            if (!exists) updatedPaths.append(candidate);
                        }
                    } else {
                        for (const QJsonValue &existingValue : previousPaths) {
                            bool remove = false;
                            if (existingValue.isObject()) {
                                for (const QJsonValue &candidateValue : paths) {
                                    if (candidateValue.isObject() && samePath(
                                            existingValue.toObject(),
                                            candidateValue.toObject())) {
                                        remove = true;
                                        break;
                                    }
                                }
                            }
                            if (!remove) updatedPaths.append(existingValue);
                        }
                    }
                    if (updatedPaths != previousPaths) {
                        keyingSet.insert(QStringLiteral("paths"), updatedPaths);
                        keyingSets[keyingSetIndex] = keyingSet;
                    }
                    if (scene.value(QStringLiteral("active_keying_set_key"))
                            .toString() != activeKey) {
                        scene.insert(QStringLiteral("active_keying_set_key"),
                                     activeKey);
                    }
                    scene.insert(QStringLiteral("keying_sets"), keyingSets);
                    scenes[sceneIndex] = scene;
                    break;
                }
                if (importedBlenderSourcePath_.isEmpty() ||
                    importedBlenderSourceSha256_.size() != 64) {
                    statusBar()->showMessage(
                        QStringLiteral("Keying Set paths can be edited only for an imported ordinary Blender file"),
                        5000);
                    return requestedKey;
                }
                QJsonObject editedScene;
                QJsonObject editedKeyingSet;
                for (const QJsonValue &sceneValue : scenes) {
                    const QJsonObject scene = sceneValue.toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneId) {
                        continue;
                    }
                    editedScene = scene;
                    for (const QJsonValue &keyingSetValue : scene.value(
                             QStringLiteral("keying_sets")).toArray()) {
                        const QJsonObject keyingSet = keyingSetValue.toObject();
                        if (keyingSet.value(QStringLiteral("key")).toString() ==
                            activeKey) {
                            editedKeyingSet = keyingSet;
                            break;
                        }
                    }
                    break;
                }
                const QString sceneName = editedScene.value(
                    QStringLiteral("name")).toString();
                if (sceneName.isEmpty() || editedKeyingSet.isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not resolve the active Blender scene or Keying Set"),
                        5000);
                    return requestedKey;
                }

                const QString sourcePath = importedBlenderSourcePath_;
                const QMessageBox::StandardButton answer = QMessageBox::warning(
                    this,
                    QStringLiteral("Edit Blender Keying Set Paths"),
                    QStringLiteral(
                        "This can update the original Blender file. If a change is needed, classiCAD creates and verifies a backup beside it.\n\n%1\n\nContinue?")
                        .arg(sourcePath),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (answer != QMessageBox::Yes) return requestedKey;

                BlenderKeyingSetEditResult editResult;
                QString errorMessage;
                if (!editBlenderKeyingSetPaths(
                        sourcePath, importedBlenderSourceSha256_, sceneName,
                        editedKeyingSet, paths, add, &editResult,
                        &errorMessage)) {
                    QMessageBox::critical(
                        this,
                        QStringLiteral("Could Not Edit Blender Keying Set"),
                        errorMessage.isEmpty()
                            ? QStringLiteral("Blender could not edit the Keying Set paths.")
                            : errorMessage);
                    return requestedKey;
                }

                for (qsizetype sceneIndex = 0; sceneIndex < scenes.size();
                     ++sceneIndex) {
                    QJsonObject scene = scenes[sceneIndex].toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneId) {
                        continue;
                    }
                    scene.insert(QStringLiteral("keying_sets"),
                                 editResult.keyingSets);
                    scene.insert(QStringLiteral("keying_sets_truncated"),
                                 editResult.keyingSetsTruncated);
                    scene.insert(QStringLiteral("active_keying_set_key"),
                                 editResult.activeKeyingSetKey);
                    scenes[sceneIndex] = scene;
                    break;
                }
                settings.importedBlenderScenes = scenes;
                importedBlenderSourceSha256_ = editResult.sourceSha256;
                const bool importedScenesChanged =
                    settings.importedBlenderScenes != originalScenes;
                if (importedScenesChanged) {
                    DocumentTransaction transaction = session_.beginTransaction();
                    if (!transaction.setSettings(settings) ||
                        !session_.commitTransaction(transaction)) {
                        if (!session_.document().setSettings(settings)) {
                            importedBlenderSourcePath_.clear();
                            importedBlenderSourceSha256_.clear();
                            const QString sourceResult =
                                editResult.backupPath.isEmpty()
                                    ? QStringLiteral("The Blender file was unchanged.")
                                    : QStringLiteral("The Blender file was changed and backed up at %1.")
                                          .arg(editResult.backupPath);
                            QMessageBox::critical(
                                this,
                                QStringLiteral("Keying Set Changed, Refresh Failed"),
                                QStringLiteral("%1 classiCAD could not refresh its Keying Set list.")
                                    .arg(sourceResult));
                            return editResult.activeKeyingSetKey;
                        }
                    }
                    markDocumentModified();
                }
                refreshCollections();
                statusBar()->showMessage(
                    editResult.backupPath.isEmpty()
                        ? QStringLiteral("Keying Set paths already match Blender")
                        : QStringLiteral("Keying Set paths updated. Backup: %1")
                              .arg(editResult.backupPath),
                    10000);
                return editResult.activeKeyingSetKey;
            };
        collectionCallbacks.editDataApiKeyframes =
            [this, activeSceneRecord](const QString &requestedKey,
                                      bool insert) -> QString {
                DocumentSettings settings = session_.document().settings();
                QJsonArray scenes = settings.importedBlenderScenes;
                const QJsonArray originalScenes = scenes;
                const QString sceneId = activeSceneRecord().value(
                    QStringLiteral("scene_id")).toString();
                if (sceneId.isEmpty() || requestedKey.isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("Choose an active Blender Keying Set first"),
                        4000);
                    return requestedKey;
                }

                QJsonObject editedScene;
                QJsonObject editedKeyingSet;
                QString viewLayerName;
                for (qsizetype sceneIndex = 0; sceneIndex < scenes.size();
                     ++sceneIndex) {
                    QJsonObject scene = scenes[sceneIndex].toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneId) {
                        continue;
                    }
                    if (scene.value(QStringLiteral("keying_sets_truncated")).toBool()) {
                        statusBar()->showMessage(
                            QStringLiteral("Keyframes are disabled because the imported Keying Set list is incomplete"),
                            5000);
                        return requestedKey;
                    }
                    for (const QJsonValue &keyingSetValue : scene.value(
                             QStringLiteral("keying_sets")).toArray()) {
                        const QJsonObject keyingSet = keyingSetValue.toObject();
                        if (keyingSet.value(QStringLiteral("key")).toString() ==
                            requestedKey) {
                            editedKeyingSet = keyingSet;
                            break;
                        }
                    }
                    if (editedKeyingSet.isEmpty()) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not resolve the active Blender Keying Set"),
                            5000);
                        return requestedKey;
                    }
                    const bool absolute = editedKeyingSet.value(
                        QStringLiteral("is_absolute")).toBool();
                    if (!absolute && !editedKeyingSet.value(
                            QStringLiteral("relative_keyframe_supported")).toBool()) {
                        statusBar()->showMessage(
                            QStringLiteral("This relative Keying Set needs Blender context that classiCAD does not represent"),
                            5000);
                        return requestedKey;
                    }
                    const QString activeLayerKey = settings.activeViewLayerKey;
                    const QJsonArray viewLayers = scene.value(
                        QStringLiteral("view_layers")).toArray();
                    for (const QJsonValue &layerValue : viewLayers) {
                        const QJsonObject layer = layerValue.toObject();
                        if (layer.value(QStringLiteral("key")).toString() ==
                            activeLayerKey) {
                            viewLayerName = layer.value(
                                QStringLiteral("name")).toString();
                            break;
                        }
                    }
                    if (viewLayerName.isEmpty() && viewLayers.size() == 1) {
                        viewLayerName = viewLayers.first().toObject().value(
                            QStringLiteral("name")).toString();
                    }
                    scene.insert(QStringLiteral("active_keying_set_key"),
                                 requestedKey);
                    scenes[sceneIndex] = scene;
                    editedScene = scene;
                    break;
                }

                if (importedBlenderSourcePath_.isEmpty() ||
                    importedBlenderSourceSha256_.size() != 64) {
                    statusBar()->showMessage(
                        QStringLiteral("Keyframe editing requires a directly opened Blender .blend file"),
                        5000);
                    return requestedKey;
                }
                const QString sceneName = editedScene.value(
                    QStringLiteral("name")).toString();
                if (sceneName.isEmpty() || editedKeyingSet.isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not resolve the Blender scene or active Keying Set"),
                        5000);
                    return requestedKey;
                }

                QStringList selectedSourceObjectIds;
                QString activeSourceObjectId;
                if (!editedKeyingSet.value(
                        QStringLiteral("is_absolute")).toBool()) {
                    for (const ObjectId objectId :
                         session_.selection().objectIds()) {
                        const SceneObject *object =
                            session_.document().object(objectId);
                        if (object == nullptr ||
                            object->blenderSourceObjectId.isEmpty() ||
                            selectedSourceObjectIds.contains(
                                object->blenderSourceObjectId)) {
                            continue;
                        }
                        selectedSourceObjectIds.append(
                            object->blenderSourceObjectId);
                    }
                    const SceneObject *activeObject =
                        session_.document().object(
                            session_.selection().activeObjectId());
                    if (activeObject != nullptr) {
                        activeSourceObjectId =
                            activeObject->blenderSourceObjectId;
                    }
                    if (selectedSourceObjectIds.isEmpty()) {
                        statusBar()->showMessage(
                            QStringLiteral("Select an imported Blender object before using this relative Keying Set"),
                            5000);
                        return requestedKey;
                    }
                    if (viewLayerName.isEmpty()) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not resolve the active Blender View Layer"),
                            5000);
                        return requestedKey;
                    }
                }

                const QString sourcePath = importedBlenderSourcePath_;
                const QString operation = insert
                    ? QStringLiteral("Insert Blender Keyframes")
                    : QStringLiteral("Delete Blender Keyframes");
                const bool absolute = editedKeyingSet.value(
                    QStringLiteral("is_absolute")).toBool();
                const QString targetDescription = absolute
                    ? QStringLiteral("every stored path in the active absolute Keying Set")
                    : QStringLiteral("the active relative Keying Set paths generated for %1 selected Blender object(s)")
                          .arg(selectedSourceObjectIds.size());
                const QMessageBox::StandardButton answer = QMessageBox::warning(
                    this,
                    operation,
                    QStringLiteral(
                        "%1 %2 at the source scene's current frame? This updates the original Blender file. classiCAD creates and verifies a backup beside it.\n\n%3")
                        .arg(insert ? QStringLiteral("Insert keys for")
                                    : QStringLiteral("Delete keys for"),
                             targetDescription,
                             sourcePath),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (answer != QMessageBox::Yes) return requestedKey;

                BlenderKeyingSetEditResult editResult;
                QString errorMessage;
                if (!editBlenderKeyingSetKeyframes(
                        sourcePath, importedBlenderSourceSha256_, sceneName,
                        editedKeyingSet, selectedSourceObjectIds,
                        activeSourceObjectId, viewLayerName, insert,
                        &editResult, &errorMessage)) {
                    QMessageBox::critical(
                        this,
                        QStringLiteral("Could Not Edit Blender Keyframes"),
                        errorMessage.isEmpty()
                            ? QStringLiteral("Blender could not edit the active Keying Set's keyframes.")
                            : errorMessage);
                    return requestedKey;
                }

                for (qsizetype sceneIndex = 0; sceneIndex < scenes.size();
                     ++sceneIndex) {
                    QJsonObject scene = scenes[sceneIndex].toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneId) {
                        continue;
                    }
                    scene.insert(QStringLiteral("keying_sets"),
                                 editResult.keyingSets);
                    scene.insert(QStringLiteral("keying_sets_truncated"),
                                 editResult.keyingSetsTruncated);
                    scene.insert(QStringLiteral("active_keying_set_key"),
                                 editResult.activeKeyingSetKey);
                    scenes[sceneIndex] = scene;
                    break;
                }
                settings.importedBlenderScenes = scenes;
                importedBlenderSourceSha256_ = editResult.sourceSha256;
                const bool importedScenesChanged =
                    settings.importedBlenderScenes != originalScenes;
                if (importedScenesChanged) {
                    DocumentTransaction transaction = session_.beginTransaction();
                    if (!transaction.setSettings(settings) ||
                        !session_.commitTransaction(transaction)) {
                        if (!session_.document().setSettings(settings)) {
                            importedBlenderSourcePath_.clear();
                            importedBlenderSourceSha256_.clear();
                            QMessageBox::critical(
                                this,
                                QStringLiteral("Keyframes Changed, Refresh Failed"),
                                QStringLiteral(
                                    "The Blender file was changed and backed up at %1, but classiCAD could not refresh its Keying Set list.")
                                    .arg(editResult.backupPath));
                            return editResult.activeKeyingSetKey;
                        }
                    }
                    markDocumentModified();
                }
                refreshCollections();

                const QString frame = QString::number(editResult.frame, 'g', 8);
                QString message;
                if (editResult.changedPathCount == 0) {
                    message = QStringLiteral("No keyframe paths changed at frame %1")
                                  .arg(frame);
                } else {
                    message = QStringLiteral("%1 %2 Keying Set paths at frame %3")
                                  .arg(insert ? QStringLiteral("Updated")
                                              : QStringLiteral("Deleted"),
                                       QString::number(editResult.changedPathCount),
                                       frame);
                }
                if (!editResult.backupPath.isEmpty()) {
                    message += QStringLiteral(". Backup: %1")
                                   .arg(editResult.backupPath);
                }
                statusBar()->showMessage(message, 10000);
                return editResult.activeKeyingSetKey;
            };
        collectionCallbacks.editDataApiDriverPaths =
            [this](const QJsonArray &paths, bool add) {
                if (paths.isEmpty()) return;
                if (importedBlenderSourcePath_.isEmpty() ||
                    importedBlenderSourceSha256_.size() != 64) {
                    statusBar()->showMessage(
                        QStringLiteral("Driver edits need the original Blender .blend file to be open"),
                        5000);
                    return;
                }
                DocumentSettings settings = session_.document().settings();
                QJsonObject dataApi = settings.importedBlenderDataApi;
                if (!dataApi.value(QStringLiteral("root")).isObject()) return;
                const QString sourcePath = importedBlenderSourcePath_;
                const QString operation = add
                    ? QStringLiteral("Add Blender Drivers")
                    : QStringLiteral("Remove Blender Drivers");
                const QString action = add
                    ? QStringLiteral("create real Blender drivers for")
                    : QStringLiteral("remove Blender drivers from");
                const QMessageBox::StandardButton answer = QMessageBox::warning(
                    this,
                    operation,
                    QStringLiteral(
                        "This will %1 the selected Data API properties in the original Blender file. classiCAD creates and verifies a backup beside it.\n\n%2\n\nContinue?")
                        .arg(action, sourcePath),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (answer != QMessageBox::Yes) return;

                BlenderDataApiDriverEditResult editResult;
                QString errorMessage;
                if (!editBlenderDataApiDrivers(
                        sourcePath, importedBlenderSourceSha256_, paths, add,
                        &editResult, &errorMessage)) {
                    QMessageBox::critical(
                        this,
                        QStringLiteral("Could Not Edit Blender Drivers"),
                        errorMessage.isEmpty()
                            ? QStringLiteral("Blender could not edit the selected drivers.")
                            : errorMessage);
                    return;
                }

                const QJsonObject originalDataApi = dataApi;
                dataApi.insert(QStringLiteral("driver_paths"),
                               editResult.driverPaths);
                dataApi.insert(QStringLiteral("driver_paths_truncated"),
                               editResult.driverPathsTruncated);
                settings.importedBlenderDataApi = dataApi;
                importedBlenderSourceSha256_ = editResult.sourceSha256;
                if (dataApi != originalDataApi) {
                    DocumentTransaction transaction = session_.beginTransaction();
                    if (!transaction.setSettings(settings) ||
                        !session_.commitTransaction(transaction)) {
                        if (!session_.document().setSettings(settings)) {
                            importedBlenderSourcePath_.clear();
                            importedBlenderSourceSha256_.clear();
                            QMessageBox::critical(
                                this,
                                QStringLiteral("Drivers Changed, Refresh Failed"),
                                QStringLiteral(
                                    "The Blender file was changed and backed up at %1, but classiCAD could not refresh its driver list.")
                                    .arg(editResult.backupPath));
                            return;
                        }
                    }
                    markDocumentModified();
                }
                refreshCollections();
                QString message = QStringLiteral("%1 Blender driver curve(s) across %2 selected path(s)")
                    .arg(QString::number(editResult.changedFCurveCount),
                         QString::number(editResult.changedPathCount));
                if (editResult.driverPathsTruncated) {
                    message += QStringLiteral(". The imported driver list is truncated");
                }
                if (!editResult.backupPath.isEmpty()) {
                    message += QStringLiteral(". Backup: %1")
                                   .arg(editResult.backupPath);
                }
                statusBar()->showMessage(message, 10000);
            };
        collectionCallbacks.activeViewLayerKey = [this]() {
            return session_.document().settings().activeViewLayerKey;
        };
        collectionCallbacks.activeCollectionSourcePath = [this]() {
            return session_.document().activeCollectionSourcePath();
        };
        collectionCallbacks.viewLayers = [this, activeSceneRecord]() {
            QVector<OutlinerViewLayerEntry> viewLayers;
            const QJsonObject scene = activeSceneRecord();
            const QString sceneName = scene.value(
                QStringLiteral("name")).toString();
            const QString activeLayerKey = session_.document().settings()
                                               .activeViewLayerKey;
            const auto sourceIdentityForObjectId = [this](ObjectId id) {
                if (!id.isValid()) return QString{};
                const SceneObject *object = session_.document().object(id);
                if (object == nullptr) return QString{};
                return object->blenderSourceObjectId.isEmpty()
                    ? QStringLiteral("classiCAD-local-object:%1")
                          .arg(id.value())
                    : object->blenderSourceObjectId;
            };
            for (const QJsonValue &layerValue : scene.value(
                     QStringLiteral("view_layers")).toArray()) {
                const QJsonObject layer = layerValue.toObject();
                const QString key = layer.value(
                    QStringLiteral("key")).toString();
                const QString name = layer.value(
                    QStringLiteral("name")).toString();
                if (key.isEmpty() || name.isEmpty()) continue;
                OutlinerViewLayerEntry entry;
                entry.key = key;
                entry.displayName = QStringLiteral("%1 / %2")
                                        .arg(sceneName, name);
                entry.baseObjectSourceIdsComplete = layer.value(
                    QStringLiteral("base_object_ids_complete")).toBool();
                for (const QJsonValue &objectIdValue : layer.value(
                         QStringLiteral("base_object_ids")).toArray()) {
                    if (objectIdValue.isString() &&
                        !objectIdValue.toString().isEmpty()) {
                        entry.baseObjectSourceIds.insert(
                            objectIdValue.toString());
                    }
                }
                const QJsonValue selectedObjectIds = layer.value(
                    QStringLiteral("selected_object_ids"));
                entry.selectedObjectSourceIdsAvailable =
                    selectedObjectIds.isArray();
                for (const QJsonValue &objectIdValue :
                     selectedObjectIds.toArray()) {
                    if (objectIdValue.isString() &&
                        !objectIdValue.toString().isEmpty()) {
                        entry.selectedObjectSourceIds.insert(
                            objectIdValue.toString());
                    }
                }
                const QJsonValue activeObjectId = layer.value(
                    QStringLiteral("active_object_id"));
                entry.activeObjectSourceIdAvailable = activeObjectId.isString();
                entry.activeObjectSourceId = activeObjectId.toString();

                if (key == activeLayerKey) {
                    entry.selectedObjectSourceIds.clear();
                    for (const ObjectId id : session_.selection().objectIds()) {
                        const QString sourceId =
                            sourceIdentityForObjectId(id);
                        if (!sourceId.isEmpty()) {
                            entry.selectedObjectSourceIds.insert(sourceId);
                        }
                    }
                    entry.selectedObjectSourceIdsAvailable = true;
                    entry.activeObjectSourceId = sourceIdentityForObjectId(
                        session_.selection().activeObjectId());
                    entry.activeObjectSourceIdAvailable = true;
                } else {
                    const auto saved = viewLayerSelections_.constFind(key);
                    if (saved != viewLayerSelections_.cend()) {
                        entry.selectedObjectSourceIds.clear();
                        for (const ObjectId id : saved->objectIds) {
                            const QString sourceId =
                                sourceIdentityForObjectId(id);
                            if (!sourceId.isEmpty()) {
                                entry.selectedObjectSourceIds.insert(sourceId);
                            }
                        }
                        entry.selectedObjectSourceIdsAvailable = true;
                        entry.activeObjectSourceId =
                            sourceIdentityForObjectId(saved->activeObjectId);
                        entry.activeObjectSourceIdAvailable = true;
                    }
                }
                viewLayers.append(std::move(entry));
            }
            return viewLayers;
        };
        collectionCallbacks.stateScopeKey = [this]() {
            return currentProjectPath_;
        };
        collectionCallbacks.loadOutlinerWorkspaceState = [this]() {
            return session_.document().outlinerWorkspaceState();
        };
        collectionCallbacks.saveOutlinerWorkspaceState =
            [this](const QJsonObject &state) {
                Document &document = session_.document();
                if (document.outlinerWorkspaceState() == state) return;
                if (document.setOutlinerWorkspaceState(state)) {
                    markDocumentModified();
                }
            };
        collectionCallbacks.loadOutlinerTreeExpansionState =
            [this](const QString &context,
                   QStringList *expandedPaths,
                   QStringList *collapsedPaths) {
                if (context.isEmpty() || expandedPaths == nullptr ||
                    collapsedPaths == nullptr) {
                    return false;
                }
                const auto &states = session_.document()
                                         .outlinerTreeExpansionByContext();
                const auto saved = states.constFind(context);
                if (saved == states.cend()) return false;
                *expandedPaths = saved->expandedPaths;
                *collapsedPaths = saved->collapsedPaths;
                return true;
            };
        collectionCallbacks.saveOutlinerTreeExpansionState =
            [this](const QString &context,
                   const QStringList &expandedPaths,
                   const QStringList &collapsedPaths) {
                if (context.isEmpty()) return;
                Document &document = session_.document();
                QHash<QString, OutlinerTreeExpansionState> states =
                    document.outlinerTreeExpansionByContext();
                const OutlinerTreeExpansionState state{
                    expandedPaths, collapsedPaths};
                if (states.value(context) == state &&
                    states.contains(context)) {
                    return;
                }
                states.insert(context, state);
                if (document.setOutlinerTreeExpansionByContext(states)) {
                    markDocumentModified();
                }
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
        collectionCallbacks.setActiveCollection =
            [this](CollectionId id, const QString &viewLayerKey,
                   const QStringList &sourcePath) {
                const Document &document = session_.document();
                const DocumentCollection *collection = document.collection(id);
                if (collection == nullptr || sourcePath.isEmpty() ||
                    sourcePath.constLast() != collection->sourceId) {
                    return;
                }
                QJsonArray path;
                for (const QString &sourceId : sourcePath) {
                    path.append(sourceId);
                }
                DocumentSettings settings = document.settings();
                if (settings.activeCollectionPathsByViewLayer.value(
                        viewLayerKey).toArray() == path) {
                    return;
                }
                settings.activeCollectionPathsByViewLayer.insert(
                    viewLayerKey, path);
                DocumentTransaction transaction = session_.beginTransaction();
                if (!transaction.setSettings(settings) ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not activate the collection"),
                        4000);
                }
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
        collectionCallbacks.interactionMode = [this]() {
            return viewport_ != nullptr ? viewport_->interactionMode() : 0;
        };
        collectionCallbacks.setObjectInteractionMode =
            [this](ObjectId id, int mode) {
                if (viewport_ == nullptr ||
                    !session_.document().isObjectEditable(id)) {
                    return;
                }
                session_.selection().setObjectIds({id}, id);
                session_.selection().setActiveObjectId(id);
                if (mode == 1 && viewport_->interactionMode() == 1) {
                    viewport_->setInteractionMode(0);
                }
                viewport_->setInteractionMode(mode);
                if (collectionsPanel_ != nullptr) {
                    collectionsPanel_->syncSelection();
                }
            };
        collectionCallbacks.hasOutlinerObjectClipboard = [this]() {
            return !outlinerObjectClipboard_.isEmpty();
        };
        collectionCallbacks.copyOutlinerObject =
            [this](const QVector<ObjectId> &requestedObjectIds) {
            const Document &document = session_.document();
            QVector<ObjectId> selectedIds;
            QSet<quint64> requestedIds;
            for (const ObjectId objectId : requestedObjectIds) {
                const SceneObject *source = document.object(objectId);
                if (source == nullptr ||
                    source->blenderLooseMeshEdgeCompanion ||
                    requestedIds.contains(objectId.value())) {
                    continue;
                }
                requestedIds.insert(objectId.value());
                selectedIds.append(objectId);
            }
            if (selectedIds.isEmpty()) return;
            outlinerObjectClipboard_.clear();
            outlinerObjectClipboardMaterials_.clear();
            outlinerObjectClipboardAnchorIdentities_.clear();
            outlinerObjectClipboardCollectionIdentities_.clear();
            outlinerCollectionClipboardRootIds_.clear();
            outlinerCollectionClipboardCollections_.clear();
            outlinerCollectionClipboardObjects_.clear();
            outlinerCollectionClipboardMaterials_.clear();
            outlinerCollectionClipboardAnchorIdentities_.clear();
            outlinerCollectionClipboardCollectionIdentities_.clear();
            for (const DocumentCollection &collection : document.collections()) {
                const QString identity = collection.sourceId.isEmpty()
                    ? QStringLiteral("classiCAD-local-collection:%1")
                          .arg(collection.id.value())
                    : QStringLiteral("%1\n%2")
                          .arg(collection.libraryPath, collection.sourceId);
                outlinerObjectClipboardCollectionIdentities_.insert(
                    collection.id.value(), identity);
            }
            QSet<quint64> copiedObjectIds;
            QStringList copiedSourceIdentities;
            for (const ObjectId selectedId : selectedIds) {
                const SceneObject *selectedObject =
                    document.object(selectedId);
                if (selectedObject == nullptr ||
                    selectedObject->blenderLooseMeshEdgeCompanion ||
                    copiedObjectIds.contains(selectedId.value())) {
                    continue;
                }
                copiedObjectIds.insert(selectedId.value());
                outlinerObjectClipboard_.append(*selectedObject);
                if (!selectedObject->blenderSourceObjectId.isEmpty()) {
                    copiedSourceIdentities.append(
                        selectedObject->blenderSourceObjectId);
                }
            }
            for (const SceneObject &candidate : document.objects()) {
                if (!candidate.blenderLooseMeshEdgeCompanion ||
                    !copiedSourceIdentities.contains(
                        candidate.blenderSourceObjectId) ||
                    copiedObjectIds.contains(candidate.id.value())) {
                    continue;
                }
                copiedObjectIds.insert(candidate.id.value());
                outlinerObjectClipboard_.append(candidate);
            }
            if (outlinerObjectClipboard_.isEmpty()) return;
            QSet<QString> materialIds;
            for (const SceneObject &object : outlinerObjectClipboard_) {
                for (const QString &materialId :
                     object.geometry.polygonMesh.materialSlotIds) {
                    materialIds.insert(materialId);
                }
                for (const QString &materialId :
                     object.nurbsSolidFaceMaterialIds) {
                    materialIds.insert(materialId);
                }
                for (const DimensionAnchorReference &anchor :
                     object.geometry.dimensionAnchors) {
                    const SceneObject *target = document.object(anchor.objectId);
                    if (target == nullptr) continue;
                    const QString identity =
                        target->blenderSourceObjectId.isEmpty()
                            ? QStringLiteral("classiCAD-local-object:%1")
                                  .arg(target->id.value())
                            : target->blenderSourceObjectId;
                    outlinerObjectClipboardAnchorIdentities_.insert(
                        anchor.objectId.value(), identity);
                }
            }
            for (const DocumentMaterial &material : document.materials()) {
                if (materialIds.contains(material.id)) {
                    outlinerObjectClipboardMaterials_.append(material);
                }
            }
            const qsizetype copiedObjectCount = std::count_if(
                outlinerObjectClipboard_.cbegin(),
                outlinerObjectClipboard_.cend(),
                [](const SceneObject &object) {
                    return !object.blenderLooseMeshEdgeCompanion;
                });
            if (!persistOutlinerClipboard()) {
                statusBar()->showMessage(
                    QStringLiteral("Copied %1 object(s), but could not save the Outliner clipboard")
                        .arg(copiedObjectCount), 4000);
            } else {
                statusBar()->showMessage(
                    QStringLiteral("Copied %1 object(s) to the Outliner clipboard")
                        .arg(copiedObjectCount), 3000);
            }
        };
        collectionCallbacks.pasteOutlinerObjects =
            [this, activeSceneRecord,
             collectionBelongsToActiveScene]() {
                if (outlinerObjectClipboard_.isEmpty()) return;
                Document &document = session_.document();
                QVector<SceneObject> copies = outlinerObjectClipboard_;
                QVector<ObjectId> sourceIds;
                sourceIds.reserve(copies.size());
                QHash<QString, QString> copiedSourceIdentity;
                for (int index = 0; index < copies.size(); ++index) {
                    SceneObject &copy = copies[index];
                    sourceIds.append(copy.id);
                    const QString sourceIdentity =
                        copy.blenderSourceObjectId.isEmpty()
                            ? QStringLiteral("classiCAD-local-object:%1")
                                  .arg(copy.id.value())
                            : copy.blenderSourceObjectId;
                    if (!copiedSourceIdentity.contains(sourceIdentity)) {
                        copiedSourceIdentity.insert(
                            sourceIdentity,
                            QStringLiteral("classiCAD-outliner-copy:%1")
                                .arg(QUuid::createUuid().toString(
                                    QUuid::WithoutBraces)));
                    }
                }
                for (SceneObject &copy : copies) {
                    const QString sourceIdentity =
                        copy.blenderSourceObjectId.isEmpty()
                            ? QStringLiteral("classiCAD-local-object:%1")
                                  .arg(copy.id.value())
                            : copy.blenderSourceObjectId;
                    copy.id = ObjectId::invalid();
                    copy.libraryPath.clear();
                    copy.workbenchRandomLibraryPath.clear();
                    copy.blenderSourceObjectId =
                        copiedSourceIdentity.value(sourceIdentity);
                    copy.viewLayerHidden = QJsonObject{};
                    copy.defaultViewLayerKey =
                        document.settings().activeViewLayerKey;
                    copy.collectionIds.clear();
                    if (copy.instanceCollectionId.isValid()) {
                        const QString instanceIdentity =
                            outlinerObjectClipboardCollectionIdentities_.value(
                                copy.instanceCollectionId.value());
                        CollectionId currentCollectionId =
                            CollectionId::invalid();
                        if (!instanceIdentity.isEmpty()) {
                            for (const DocumentCollection &collection :
                                 document.collections()) {
                                const QString identity =
                                    collection.sourceId.isEmpty()
                                        ? QStringLiteral(
                                              "classiCAD-local-collection:%1")
                                              .arg(collection.id.value())
                                        : QStringLiteral("%1\n%2")
                                              .arg(collection.libraryPath,
                                                   collection.sourceId);
                                if (identity == instanceIdentity) {
                                    currentCollectionId = collection.id;
                                    break;
                                }
                            }
                        }
                        copy.instanceCollectionId = currentCollectionId;
                        if (!currentCollectionId.isValid()) {
                            copy.instanceTransform = identityAffineTransform3D;
                        }
                    }
                }
                for (int index = 0; index < copies.size(); ++index) {
                    SceneObject &copy = copies[index];
                    const SceneObject &source =
                        outlinerObjectClipboard_[index];
                    if (!source.blenderParentObjectId.isEmpty()) {
                        copy.blenderParentObjectId = copiedSourceIdentity.value(
                            source.blenderParentObjectId);
                        if (copy.blenderParentObjectId.isEmpty()) {
                            const bool parentStillExists = std::any_of(
                                document.objects().cbegin(),
                                document.objects().cend(),
                                [&source](const SceneObject &candidate) {
                                    const QString identity =
                                        candidate.blenderSourceObjectId.isEmpty()
                                            ? QStringLiteral(
                                                  "classiCAD-local-object:%1")
                                                  .arg(candidate.id.value())
                                            : candidate.blenderSourceObjectId;
                                    return identity ==
                                           source.blenderParentObjectId;
                                });
                            if (parentStillExists) {
                                copy.blenderParentObjectId =
                                    source.blenderParentObjectId;
                            }
                        }
                    }
                }

                const QJsonObject activeScene = activeSceneRecord();
                const QString activeSceneId = activeScene.value(
                    QStringLiteral("scene_id")).toString();
                const QString activeSceneName = activeScene.value(
                    QStringLiteral("name")).toString();
                CollectionId destinationCollection =
                    document.activeCollectionId();
                const DocumentCollection *activeCollection =
                    document.collection(destinationCollection);
                if (activeCollection == nullptr ||
                    !activeCollection->libraryPath.isEmpty() ||
                    !collectionBelongsToActiveScene(*activeCollection)) {
                    destinationCollection = CollectionId::invalid();
                }
                int destinationRootOrder = std::numeric_limits<int>::max();
                if (!destinationCollection.isValid()) {
                    for (const DocumentCollection &collection :
                         document.collections()) {
                        if (!collection.parentIds.isEmpty() ||
                            !collectionBelongsToActiveScene(collection)) {
                            continue;
                        }
                        const int rootOrder = collection.rootOrder < 0
                            ? std::numeric_limits<int>::max() - 1
                            : collection.rootOrder;
                        if (rootOrder < destinationRootOrder) {
                            destinationRootOrder = rootOrder;
                            destinationCollection = collection.id;
                        }
                    }
                }
                if (destinationCollection.isValid()) {
                    for (SceneObject &copy : copies) {
                        copy.collectionIds = {destinationCollection};
                    }
                }

                QHash<QString, QString> materialIdRemap;
                QVector<DocumentMaterial> materialsToAdd;
                const auto sameMaterial = [](const DocumentMaterial &left,
                                             const DocumentMaterial &right) {
                    if (left.name != right.name ||
                        left.libraryPath != right.libraryPath ||
                        left.baseColorSceneLinear !=
                            right.baseColorSceneLinear ||
                        left.roughness != right.roughness ||
                        left.metallic != right.metallic ||
                        left.alpha != right.alpha ||
                        left.imageTextures.size() !=
                            right.imageTextures.size()) {
                        return false;
                    }
                    for (int index = 0;
                         index < left.imageTextures.size(); ++index) {
                        const DocumentImageTextureReference &a =
                            left.imageTextures[index];
                        const DocumentImageTextureReference &b =
                            right.imageTextures[index];
                        if (a.nodeName != b.nodeName ||
                            a.nodeType != b.nodeType ||
                            a.imageName != b.imageName ||
                            a.imageLibraryPath != b.imageLibraryPath ||
                            a.filepath != b.filepath ||
                            a.colorSpace != b.colorSpace ||
                            a.sourceTransformId != b.sourceTransformId ||
                            a.alphaMode != b.alphaMode ||
                            a.interpolation != b.interpolation ||
                            a.extension != b.extension ||
                            a.packedDataBase64 != b.packedDataBase64 ||
                            a.packed != b.packed ||
                            a.sourceFileExists != b.sourceFileExists ||
                            a.sceneLinearFloatDataBase64 !=
                                b.sceneLinearFloatDataBase64 ||
                            a.sceneLinearFloatWidth !=
                                b.sceneLinearFloatWidth ||
                            a.sceneLinearFloatHeight !=
                                b.sceneLinearFloatHeight ||
                            a.tiles.size() != b.tiles.size()) {
                            return false;
                        }
                        for (int tileIndex = 0; tileIndex < a.tiles.size();
                             ++tileIndex) {
                            const DocumentImageTextureTile &leftTile =
                                a.tiles[tileIndex];
                            const DocumentImageTextureTile &rightTile =
                                b.tiles[tileIndex];
                            if (leftTile.number != rightTile.number ||
                                leftTile.filepath != rightTile.filepath ||
                                leftTile.sourceFileExists !=
                                    rightTile.sourceFileExists ||
                                leftTile.packedDataBase64 !=
                                    rightTile.packedDataBase64 ||
                                leftTile.sceneLinearFloatDataBase64 !=
                                    rightTile.sceneLinearFloatDataBase64 ||
                                leftTile.sceneLinearFloatWidth !=
                                    rightTile.sceneLinearFloatWidth ||
                                leftTile.sceneLinearFloatHeight !=
                                    rightTile.sceneLinearFloatHeight) {
                                return false;
                            }
                        }
                    }
                    return true;
                };
                for (DocumentMaterial material :
                     outlinerObjectClipboardMaterials_) {
                    const QString sourceMaterialId = material.id;
                    const auto existing = std::find_if(
                        document.materials().cbegin(),
                        document.materials().cend(),
                        [&material](const DocumentMaterial &candidate) {
                            return candidate.id == material.id;
                        });
                    if (existing != document.materials().cend() &&
                        sameMaterial(*existing, material)) {
                        materialIdRemap.insert(sourceMaterialId, material.id);
                        continue;
                    }
                    if (existing != document.materials().cend()) {
                        material.id = QStringLiteral(
                            "classiCAD-outliner-copy-material:%1")
                            .arg(QUuid::createUuid().toString(
                                QUuid::WithoutBraces));
                    }
                    materialIdRemap.insert(sourceMaterialId, material.id);
                    materialsToAdd.append(std::move(material));
                }
                for (SceneObject &copy : copies) {
                    for (QString &materialId :
                         copy.geometry.polygonMesh.materialSlotIds) {
                        materialId = materialIdRemap.value(materialId,
                                                            materialId);
                    }
                    for (auto materialIt =
                             copy.nurbsSolidFaceMaterialIds.begin();
                         materialIt !=
                             copy.nurbsSolidFaceMaterialIds.end();
                         ++materialIt) {
                        materialIt.value() = materialIdRemap.value(
                            materialIt.value(), materialIt.value());
                    }
                }

                QSet<QString> existingObjectNames;
                for (const SceneObject &object : document.objects()) {
                    if (!object.displayName.isEmpty()) {
                        existingObjectNames.insert(object.displayName);
                    }
                }
                for (SceneObject &copy : copies) {
                    const QString baseName = copy.displayName.trimmed().isEmpty()
                        ? QStringLiteral("Object") : copy.displayName.trimmed();
                    QString uniqueName = baseName;
                    for (int suffix = 1;
                         existingObjectNames.contains(uniqueName); ++suffix) {
                        uniqueName = QStringLiteral("%1.%2")
                            .arg(baseName)
                            .arg(suffix, 3, 10, QLatin1Char('0'));
                    }
                    copy.displayName = uniqueName;
                    existingObjectNames.insert(uniqueName);
                }

                DocumentTransaction transaction = session_.beginTransaction();
                for (const DocumentMaterial &material : materialsToAdd) {
                    if (!transaction.addMaterial(material)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not copy object materials"),
                            4000);
                        return;
                    }
                }
                const QVector<ObjectId> pastedIds = transaction.insertObjects(
                    document.size(), copies);
                if (pastedIds.size() != copies.size()) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not paste Outliner objects"),
                        4000);
                    return;
                }
                QHash<quint64, ObjectId> pastedBySourceId;
                for (int index = 0; index < sourceIds.size(); ++index) {
                    pastedBySourceId.insert(sourceIds[index].value(),
                                            pastedIds[index]);
                }
                for (int index = 0; index < pastedIds.size(); ++index) {
                    Shape *pastedGeometry =
                        transaction.editGeometry(pastedIds[index]);
                    if (pastedGeometry == nullptr) continue;
                    for (DimensionAnchorReference &anchor :
                         pastedGeometry->dimensionAnchors) {
                        const auto mapped = pastedBySourceId.constFind(
                            anchor.objectId.value());
                        if (mapped != pastedBySourceId.cend()) {
                            anchor.objectId = mapped.value();
                            continue;
                        }
                        const QString anchorIdentity =
                            outlinerObjectClipboardAnchorIdentities_.value(
                                anchor.objectId.value());
                        ObjectId currentTargetId = ObjectId::invalid();
                        if (!anchorIdentity.isEmpty()) {
                            for (const SceneObject &candidate :
                                 document.objects()) {
                                const QString identity =
                                    candidate.blenderSourceObjectId.isEmpty()
                                        ? QStringLiteral(
                                              "classiCAD-local-object:%1")
                                              .arg(candidate.id.value())
                                        : candidate.blenderSourceObjectId;
                                if (identity == anchorIdentity) {
                                    currentTargetId = candidate.id;
                                    break;
                                }
                            }
                        }
                        if (currentTargetId.isValid()) {
                            anchor.objectId = currentTargetId;
                        } else {
                            anchor = DimensionAnchorReference{};
                        }
                    }
                }
                if (!activeSceneId.isEmpty() && !activeSceneName.isEmpty()) {
                    DocumentSettings settings = document.settings();
                    QJsonArray scenes = settings.importedBlenderScenes;
                    for (qsizetype sceneIndex = 0; sceneIndex < scenes.size();
                         ++sceneIndex) {
                        QJsonObject scene = scenes[sceneIndex].toObject();
                        if (scene.value(QStringLiteral("scene_id")).toString() !=
                            activeSceneId) {
                            continue;
                        }
                        QJsonArray objectIds = scene.value(
                            QStringLiteral("object_ids")).toArray();
                        for (const SceneObject &copy : copies) {
                            if (!copy.blenderLooseMeshEdgeCompanion &&
                                !copy.blenderSourceObjectId.isEmpty()) {
                                objectIds.append(copy.blenderSourceObjectId);
                            }
                        }
                        scene.insert(QStringLiteral("object_ids"), objectIds);
                        scenes[sceneIndex] = scene;
                        settings.importedBlenderScenes = scenes;
                        if (!transaction.setSettings(settings)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not register pasted objects in the active scene"),
                                4000);
                            return;
                        }
                        break;
                    }
                }
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not paste Outliner objects"),
                        4000);
                    return;
                }
                QVector<ObjectId> selectedIds;
                for (int index = 0; index < copies.size(); ++index) {
                    if (!copies[index].blenderLooseMeshEdgeCompanion) {
                        selectedIds.append(pastedIds[index]);
                    }
                }
                if (!selectedIds.isEmpty()) {
                    const ObjectId active = selectedIds.back();
                    session_.selection().setObjectIds(selectedIds, active);
                    session_.selection().setActiveObjectId(active);
                }
                statusBar()->showMessage(
                    QStringLiteral("Pasted %1 object(s) from the Outliner clipboard")
                        .arg(selectedIds.size()), 4000);
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
        collectionCallbacks.selectOutlinerObjects =
            [this](const QVector<ObjectId> &objectIds) {
                SelectionModel &selection = session_.selection();
                QVector<ObjectId> selectedIds = selection.objectIds();
                const QVector<ObjectId> previousIds = selectedIds;
                for (const ObjectId id : objectIds) {
                    if (session_.document().isObjectEditable(id) &&
                        !selectedIds.contains(id)) {
                        selectedIds.append(id);
                    }
                }
                if (selectedIds == previousIds) return;

                const ObjectId previousActive = selection.activeObjectId();
                ObjectId primary = selection.primaryObjectId();
                if (!selectedIds.contains(primary)) {
                    primary = selectedIds.isEmpty()
                        ? ObjectId::invalid() : selectedIds.back();
                }
                selection.setObjectIds(selectedIds, primary);
                selection.setActiveObjectId(previousActive);
            };
        const auto blenderDataTypeKey = [](const SceneObject &object) {
            if (!object.blenderSourceDataType.isEmpty()) {
                return object.blenderSourceDataType;
            }
            const QString &objectType = object.blenderSourceObjectType;
            if (objectType == QLatin1String("CURVE") ||
                objectType == QLatin1String("SURFACE") ||
                objectType == QLatin1String("FONT")) {
                return QStringLiteral("Curve");
            }
            if (objectType == QLatin1String("MESH")) {
                return QStringLiteral("Mesh");
            }
            if (objectType == QLatin1String("META")) {
                return QStringLiteral("MetaBall");
            }
            if (objectType == QLatin1String("GPENCIL") ||
                objectType == QLatin1String("GREASEPENCIL")) {
                return QStringLiteral("GreasePencil");
            }
            if (objectType == QLatin1String("POINTCLOUD")) {
                return QStringLiteral("PointCloud");
            }
            if (objectType == QLatin1String("LIGHT_PROBE")) {
                return QStringLiteral("LightProbe");
            }
            if (objectType.isEmpty() || objectType == QLatin1String("EMPTY")) {
                return QString{};
            }
            QString dataType = objectType.toLower();
            if (!dataType.isEmpty()) {
                dataType[0] = dataType[0].toUpper();
            }
            return dataType;
        };
        const auto linkedObjectDataUsers =
            [this, blenderDataTypeKey](const QVector<ObjectId> &sourceIds) {
            QVector<ObjectId> users;
            const Document &document = session_.document();
            QSet<QString> sourceDataKeys;
            for (const ObjectId sourceId : sourceIds) {
                const SceneObject *source = document.object(sourceId);
                if (source == nullptr ||
                    source->blenderSourceDataName.isEmpty()) {
                    continue;
                }
                const QString dataType = blenderDataTypeKey(*source);
                if (dataType.isEmpty()) continue;
                sourceDataKeys.insert(
                    dataType + QChar(0x1f) +
                    source->blenderSourceDataName);
            }
            if (sourceDataKeys.isEmpty()) return users;
            for (const SceneObject &candidate : document.objects()) {
                const QString dataType = blenderDataTypeKey(candidate);
                const QString candidateKey =
                    dataType + QChar(0x1f) +
                    candidate.blenderSourceDataName;
                if (!sourceDataKeys.contains(candidateKey) ||
                    !document.isObjectEditable(candidate.id)) {
                    continue;
                }
                users.append(candidate.id);
            }
            return users;
        };
        collectionCallbacks.hasLinkedObjectData =
            [linkedObjectDataUsers](ObjectId id) {
                return !linkedObjectDataUsers({id}).isEmpty();
            };
        collectionCallbacks.selectLinkedObjectData =
            [this, linkedObjectDataUsers](
                const QVector<ObjectId> &sourceIds) {
                const QVector<ObjectId> users =
                    linkedObjectDataUsers(sourceIds);
                if (users.isEmpty()) return;
                QVector<ObjectId> selectedIds = session_.selection().objectIds();
                for (const ObjectId user : users) {
                    if (!selectedIds.contains(user)) selectedIds.append(user);
                }
                ObjectId active = session_.selection().activeObjectId();
                if (!selectedIds.contains(active)) {
                    active = sourceIds.isEmpty() ||
                                     !users.contains(sourceIds.front())
                        ? users.front() : sourceIds.front();
                }
                session_.selection().setObjectIds(selectedIds, active);
                session_.selection().setActiveObjectId(active);
            };
        const auto linkedMaterialUsers =
            [this](const QStringList &materialIds) {
            QVector<ObjectId> users;
            QSet<QString> materialIdSet;
            for (const QString &materialId : materialIds) {
                if (!materialId.isEmpty()) materialIdSet.insert(materialId);
            }
            if (materialIdSet.isEmpty()) return users;
            const Document &document = session_.document();
            for (const SceneObject &candidate : document.objects()) {
                bool usesMaterial = false;
                if (candidate.geometry.geometryType ==
                    GeometryType::PolygonMesh) {
                    usesMaterial = std::any_of(
                        candidate.geometry.polygonMesh.materialSlotIds.cbegin(),
                        candidate.geometry.polygonMesh.materialSlotIds.cend(),
                        [&materialIdSet](const QString &materialId) {
                            return materialIdSet.contains(materialId);
                        });
                }
                if (!usesMaterial) {
                    const QList<QString> solidMaterialIds =
                        candidate.nurbsSolidFaceMaterialIds.values();
                    usesMaterial = std::any_of(
                        solidMaterialIds.cbegin(), solidMaterialIds.cend(),
                        [&materialIdSet](const QString &materialId) {
                            return materialIdSet.contains(materialId);
                        });
                }
                if (usesMaterial && document.isObjectEditable(candidate.id)) {
                    users.append(candidate.id);
                }
            }
            return users;
        };
        collectionCallbacks.hasLinkedMaterial =
            [linkedMaterialUsers](const QString &materialId) {
                return !linkedMaterialUsers({materialId}).isEmpty();
            };
        collectionCallbacks.selectLinkedMaterial =
            [this, linkedMaterialUsers](const QStringList &materialIds,
                                        ObjectId clickedObjectId) {
                const QVector<ObjectId> users =
                    linkedMaterialUsers(materialIds);
                if (users.isEmpty()) return;
                QVector<ObjectId> selectedIds = session_.selection().objectIds();
                for (const ObjectId user : users) {
                    if (!selectedIds.contains(user)) selectedIds.append(user);
                }
                ObjectId active = session_.selection().activeObjectId();
                if (!selectedIds.contains(active)) {
                    active = users.contains(clickedObjectId)
                        ? clickedObjectId : users.front();
                }
                session_.selection().setObjectIds(selectedIds, active);
                session_.selection().setActiveObjectId(active);
            };
        collectionCallbacks.deselectSelectedObjects =
            [this](const QVector<ObjectId> &objectIds) {
            SelectionModel &selection = session_.selection();
            for (const ObjectId id : objectIds) {
                selection.remove(id);
            }
        };
        collectionCallbacks.createCollection =
            [this, activeSceneRecord](const QString &name,
                                      CollectionId parentId) {
                DocumentCollection collection;
                collection.name = name;
                if (parentId.isValid()) {
                    collection.parentIds = {parentId};
                    const DocumentCollection *parentCollection =
                        session_.document().collection(parentId);
                    if (parentCollection != nullptr) {
                        collection.colorTag = parentCollection->colorTag;
                    }
                }
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
        collectionCallbacks.linkCollectionsToActiveScene =
            [this, activeSceneRecord, collectionBelongsToScene,
             updateSceneCollectionIds](
                const QVector<CollectionId> &requestedRootIds) {
                const Document &document = session_.document();
                const QJsonObject scene = activeSceneRecord();
                const QString sceneId = scene.value(
                    QStringLiteral("scene_id")).toString();
                const QString sceneName = scene.value(
                    QStringLiteral("name")).toString();
                if (sceneId.isEmpty() || sceneName.isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("No active Blender scene to link into"),
                        4000);
                    return;
                }
                const QVector<DocumentCollection> sourceCollections =
                    document.collections();
                QHash<quint64, DocumentCollection> collectionsById;
                for (const DocumentCollection &collection : sourceCollections) {
                    collectionsById.insert(collection.id.value(), collection);
                }
                QVector<CollectionId> hierarchy;
                QSet<quint64> hierarchyValues;
                QSet<quint64> seenRoots;
                for (const CollectionId rootId : requestedRootIds) {
                    if (!rootId.isValid() ||
                        !collectionsById.contains(rootId.value()) ||
                        seenRoots.contains(rootId.value())) {
                        continue;
                    }
                    seenRoots.insert(rootId.value());
                    hierarchy.append(rootId);
                    hierarchyValues.insert(rootId.value());
                }
                if (hierarchy.isEmpty()) return;
                const QVector<CollectionId> selectedRoots = hierarchy;
                for (qsizetype index = 0; index < hierarchy.size(); ++index) {
                    const CollectionId parentId = hierarchy[index];
                    for (const DocumentCollection &candidate :
                         sourceCollections) {
                        if (candidate.parentIds.contains(parentId) &&
                            !hierarchyValues.contains(candidate.id.value())) {
                            hierarchyValues.insert(candidate.id.value());
                            hierarchy.append(candidate.id);
                        }
                    }
                    if (hierarchy.size() > 20000) {
                        statusBar()->showMessage(
                            QStringLiteral("Collection hierarchy is too large to link"),
                            4000);
                        return;
                    }
                }

                struct SceneCollectionPath {
                    CollectionId collectionId = CollectionId::invalid();
                    QStringList sourcePath;
                    QSet<quint64> ancestors;
                };
                QVector<SceneCollectionPath> paths;
                QHash<quint64, QSet<QString>> visitedPaths;
                for (const CollectionId rootId : selectedRoots) {
                    const DocumentCollection root =
                        collectionsById.value(rootId.value());
                    if (root.sourceId.isEmpty()) continue;
                    visitedPaths[rootId.value()].insert(root.sourceId);
                    paths.append(
                        {rootId, {root.sourceId}, {rootId.value()}});
                }
                if (paths.isEmpty()) return;
                for (qsizetype index = 0; index < paths.size(); ++index) {
                    const SceneCollectionPath current = paths[index];
                    for (const DocumentCollection &candidate :
                         sourceCollections) {
                        if (!hierarchyValues.contains(candidate.id.value()) ||
                            !candidate.parentIds.contains(
                                current.collectionId) ||
                            current.ancestors.contains(candidate.id.value()) ||
                            candidate.sourceId.isEmpty()) {
                            continue;
                        }
                        QStringList childPath = current.sourcePath;
                        childPath.append(candidate.sourceId);
                        const QString pathKey = childPath.join(
                            QLatin1Char('\x1f'));
                        if (visitedPaths[candidate.id.value()].contains(
                                pathKey)) {
                            continue;
                        }
                        visitedPaths[candidate.id.value()].insert(pathKey);
                        SceneCollectionPath child{
                            candidate.id, childPath, current.ancestors};
                        child.ancestors.insert(candidate.id.value());
                        paths.append(std::move(child));
                        if (paths.size() > 20000) {
                            statusBar()->showMessage(
                                QStringLiteral("Collection has too many scene paths to link"),
                                4000);
                            return;
                        }
                    }
                }

                QStringList layerKeys;
                for (const QJsonValue &layerValue : scene.value(
                         QStringLiteral("view_layers")).toArray()) {
                    const QString key = layerValue.toObject().value(
                        QStringLiteral("key")).toString();
                    if (!key.isEmpty() && !layerKeys.contains(key)) {
                        layerKeys.append(key);
                    }
                }
                DocumentTransaction transaction = session_.beginTransaction();
                for (const CollectionId id : hierarchy) {
                    const DocumentCollection source =
                        collectionsById.value(id.value());
                    QStringList sceneNames = source.sourceSceneNames;
                    if (!sceneNames.contains(sceneName) ||
                        !source.sceneMembershipExplicit) {
                        if (!sceneNames.contains(sceneName)) {
                            sceneNames.append(sceneName);
                        }
                        if (!transaction.setCollectionSceneNames(id,
                                                                 sceneNames)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not link collection to scene"),
                                4000);
                            return;
                        }
                    }
                }
                for (const SceneCollectionPath &path : paths) {
                    for (const QString &layerKey : layerKeys) {
                        if (!transaction.ensureCollectionViewLayerState(
                                path.collectionId, layerKey,
                                path.sourcePath)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not create scene collection state"),
                                4000);
                            return;
                        }
                    }
                }
                QStringList linkedSourceIds;
                for (const DocumentCollection &collection :
                     document.collections()) {
                    if (collectionBelongsToScene(collection, sceneName) &&
                        !collection.sourceId.isEmpty()) {
                        linkedSourceIds.append(collection.sourceId);
                    }
                }
                if (!updateSceneCollectionIds(transaction, sceneId,
                                              linkedSourceIds)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update scene collection links"),
                        4000);
                    return;
                }
                if (transaction.changes().isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("Collection is already linked to the active scene"),
                        4000);
                    return;
                }
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not link collection to scene"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.unlinkCollections =
            [this, activeSceneRecord, collectionBelongsToScene,
             updateSceneCollectionIds](
                const QVector<OutlinerCollectionContext> &requestedTargets) {
                const Document &document = session_.document();
                const QJsonObject scene = activeSceneRecord();
                const QString sceneId = scene.value(
                    QStringLiteral("scene_id")).toString();
                const QString sceneName = scene.value(
                    QStringLiteral("name")).toString();
                if (sceneId.isEmpty() || sceneName.isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("No active Blender scene to unlink from"),
                        4000);
                    return;
                }
                const QVector<DocumentCollection> sourceCollections =
                    document.collections();
                QHash<quint64, DocumentCollection> collectionsById;
                for (const DocumentCollection &collection : sourceCollections) {
                    collectionsById.insert(collection.id.value(), collection);
                }
                QVector<OutlinerCollectionContext> targets;
                QSet<QString> seenTargets;
                for (const OutlinerCollectionContext &target :
                     requestedTargets) {
                    if (!target.collectionId.isValid() ||
                        !collectionsById.contains(
                            target.collectionId.value())) {
                        continue;
                    }
                    const QString key =
                        QString::number(target.collectionId.value()) +
                        QLatin1Char('\x1f') +
                        QString::number(target.parentCollectionId.value());
                    if (seenTargets.contains(key)) continue;
                    seenTargets.insert(key);
                    targets.append(target);
                }
                if (targets.isEmpty()) return;
                QVector<CollectionId> sceneRoots;
                for (const DocumentCollection &collection : sourceCollections) {
                    if (!collectionBelongsToScene(collection, sceneName)) {
                        continue;
                    }
                    bool hasSceneParent = false;
                    for (const CollectionId candidateParentId :
                         collection.parentIds) {
                        const auto candidateParent =
                            collectionsById.constFind(
                                candidateParentId.value());
                        if (candidateParent != collectionsById.cend() &&
                            collectionBelongsToScene(candidateParent.value(),
                                                     sceneName)) {
                            hasSceneParent = true;
                            break;
                        }
                    }
                    if (!hasSceneParent) sceneRoots.append(collection.id);
                }

                DocumentTransaction transaction = session_.beginTransaction();
                QHash<quint64, QVector<CollectionId>> updatedParentIds;
                QSet<quint64> unlinkedSceneRoots;
                for (const OutlinerCollectionContext &target : targets) {
                    const auto selected = collectionsById.constFind(
                        target.collectionId.value());
                    if (selected == collectionsById.cend()) continue;
                    if (target.parentCollectionId.isValid()) {
                        QVector<CollectionId> &parents =
                            updatedParentIds[target.collectionId.value()];
                        if (parents.isEmpty()) parents = selected->parentIds;
                        if (!parents.contains(target.parentCollectionId)) {
                            continue;
                        }
                        parents.removeAll(target.parentCollectionId);
                    } else if (sceneRoots.contains(target.collectionId)) {
                        unlinkedSceneRoots.insert(
                            target.collectionId.value());
                    }
                }
                for (auto it = updatedParentIds.cbegin();
                     it != updatedParentIds.cend(); ++it) {
                    const DocumentCollection &collection =
                        collectionsById.value(it.key());
                    if (collection.parentIds == it.value()) continue;
                    if (!transaction.setCollectionParents(
                            collection.id, it.value())) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not unlink selected collections from their parents"),
                            4000);
                        return;
                    }
                }
                for (qsizetype index = sceneRoots.size() - 1;
                     index >= 0; --index) {
                    if (unlinkedSceneRoots.contains(
                            sceneRoots[index].value())) {
                        sceneRoots.removeAt(index);
                    }
                }

                QSet<quint64> reachable;
                QVector<CollectionId> pending = sceneRoots;
                for (const CollectionId rootId : sceneRoots) {
                    reachable.insert(rootId.value());
                }
                const QVector<DocumentCollection> updatedCollections =
                    document.collections();
                for (qsizetype index = 0; index < pending.size(); ++index) {
                    const CollectionId parentCollectionId = pending[index];
                    for (const DocumentCollection &candidate :
                         updatedCollections) {
                        if (candidate.parentIds.contains(
                                parentCollectionId) &&
                            !reachable.contains(candidate.id.value())) {
                            reachable.insert(candidate.id.value());
                            pending.append(candidate.id);
                        }
                    }
                    if (pending.size() > 20000) {
                        statusBar()->showMessage(
                            QStringLiteral("Collection hierarchy is too large to unlink"),
                            4000);
                        return;
                    }
                }

                QStringList layerKeys;
                for (const QJsonValue &layerValue : scene.value(
                         QStringLiteral("view_layers")).toArray()) {
                    const QString key = layerValue.toObject().value(
                        QStringLiteral("key")).toString();
                    if (!key.isEmpty() && !layerKeys.contains(key)) {
                        layerKeys.append(key);
                    }
                }
                QStringList linkedSourceIds;
                for (const DocumentCollection &snapshot : sourceCollections) {
                    const DocumentCollection *collection =
                        document.collection(snapshot.id);
                    if (collection == nullptr) continue;
                    const bool remainsLinked =
                        reachable.contains(collection->id.value());
                    QStringList sceneNames = collection->sourceSceneNames;
                    const bool hasSceneName = sceneNames.contains(sceneName);
                    if (remainsLinked) {
                        if (!hasSceneName ||
                            !collection->sceneMembershipExplicit) {
                            if (!hasSceneName) sceneNames.append(sceneName);
                            if (!transaction.setCollectionSceneNames(
                                    collection->id, sceneNames)) {
                                statusBar()->showMessage(
                                    QStringLiteral("Could not preserve scene collection membership"),
                                    4000);
                                return;
                            }
                        }
                        if (!collection->sourceId.isEmpty()) {
                            linkedSourceIds.append(collection->sourceId);
                        }
                        continue;
                    }
                    if (hasSceneName ||
                        (!collection->sceneMembershipExplicit &&
                         collection->sourceSceneNames.isEmpty())) {
                        sceneNames.removeAll(sceneName);
                        if (!transaction.setCollectionSceneNames(
                                collection->id, sceneNames) ||
                            !transaction.removeCollectionViewLayerStates(
                                collection->id, layerKeys)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not remove scene collection membership"),
                                4000);
                            return;
                        }
                    }
                }
                if (!updateSceneCollectionIds(transaction, sceneId,
                                              linkedSourceIds)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update scene collection links"),
                        4000);
                    return;
                }
                if (transaction.changes().isEmpty()) {
                    statusBar()->showMessage(
                        QStringLiteral("Collection is not linked to the active scene"),
                        4000);
                    return;
                }
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not unlink collection"), 4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.instanceCollectionsToScene =
            [this, activeSceneRecord, collectionBelongsToActiveScene](
                const QVector<CollectionId> &requestedTargetIds,
                const OutlinerCollectionContext &activeCollectionContext) {
                const Document &document = session_.document();
                const QVector<DocumentCollection> allCollections =
                    document.collections();
                QVector<DocumentCollection> targetCollections;
                QSet<quint64> seenTargetIds;
                QSet<quint64> targetHierarchy;
                for (const CollectionId targetId : requestedTargetIds) {
                    if (!targetId.isValid() ||
                        seenTargetIds.contains(targetId.value())) {
                        continue;
                    }
                    const DocumentCollection *target =
                        document.collection(targetId);
                    if (target == nullptr) continue;
                    seenTargetIds.insert(targetId.value());
                    targetCollections.append(*target);
                    QVector<CollectionId> hierarchy{targetId};
                    for (qsizetype index = 0; index < hierarchy.size(); ++index) {
                        const CollectionId parentId = hierarchy[index];
                        targetHierarchy.insert(parentId.value());
                        for (const DocumentCollection &candidate :
                             allCollections) {
                            if (candidate.parentIds.contains(parentId) &&
                                !targetHierarchy.contains(
                                    candidate.id.value())) {
                                hierarchy.append(candidate.id);
                            }
                        }
                    }
                }
                if (targetCollections.isEmpty()) return;

                const QJsonObject activeScene = activeSceneRecord();
                const QString sceneId = activeScene.value(
                    QStringLiteral("scene_id")).toString();
                const QString sceneName = activeScene.value(
                    QStringLiteral("name")).toString();
                Point3D sceneCursorLocation;
                const QJsonArray sceneCursor = activeScene.value(
                    QStringLiteral("cursor_location")).toArray();
                if (sceneCursor.size() == 3 &&
                    sceneCursor[0].isDouble() &&
                    sceneCursor[1].isDouble() &&
                    sceneCursor[2].isDouble() &&
                    std::isfinite(sceneCursor[0].toDouble()) &&
                    std::isfinite(sceneCursor[1].toDouble()) &&
                    std::isfinite(sceneCursor[2].toDouble())) {
                    sceneCursorLocation = {sceneCursor[0].toDouble(),
                                           sceneCursor[1].toDouble(),
                                           sceneCursor[2].toDouble()};
                }
                QString sceneCursorKey =
                    QStringLiteral("classiCAD-default-scene");
                if (!activeScene.isEmpty()) {
                    const QString activeSceneId = activeScene.value(
                        QStringLiteral("scene_id")).toString();
                    const QString activeSceneName = activeScene.value(
                        QStringLiteral("name")).toString();
                    if (!activeSceneId.isEmpty()) {
                        sceneCursorKey = activeSceneId;
                    } else if (!activeSceneName.isEmpty()) {
                        sceneCursorKey = QStringLiteral("blender-scene-name:%1")
                                             .arg(activeSceneName);
                    } else {
                        const QJsonArray scenes =
                            document.settings().importedBlenderScenes;
                        for (qsizetype sceneIndex = 0;
                             sceneIndex < scenes.size(); ++sceneIndex) {
                            if (scenes[sceneIndex].toObject() == activeScene) {
                                sceneCursorKey =
                                    QStringLiteral("blender-scene-index:%1")
                                        .arg(sceneIndex);
                                break;
                            }
                        }
                    }
                }
                const auto savedSceneCursor =
                    document.sceneCursorLocationsByScene().constFind(
                        sceneCursorKey);
                if (savedSceneCursor !=
                    document.sceneCursorLocationsByScene().cend()) {
                    sceneCursorLocation = savedSceneCursor.value();
                }
                const QStringList activeCollectionPath =
                    document.activeCollectionSourcePath();
                CollectionId destinationId = document.activeCollectionId();
                CollectionId activeParentId = CollectionId::invalid();
                if (activeCollectionPath.size() > 1) {
                    const QString parentSourceId = activeCollectionPath.at(
                        activeCollectionPath.size() - 2);
                    for (const DocumentCollection &candidate : allCollections) {
                        if (candidate.sourceId == parentSourceId) {
                            activeParentId = candidate.id;
                            break;
                        }
                    }
                } else if (activeCollectionPath.isEmpty()) {
                    destinationId = activeCollectionContext.collectionId;
                    activeParentId =
                        activeCollectionContext.parentCollectionId;
                }
                while (destinationId.isValid() &&
                       targetHierarchy.contains(destinationId.value())) {
                    const DocumentCollection *activeCollection =
                        document.collection(destinationId);
                    if (activeCollection == nullptr) {
                        destinationId = CollectionId::invalid();
                        break;
                    }
                    if (!activeParentId.isValid() ||
                        !activeCollection->parentIds.contains(activeParentId)) {
                        activeParentId = activeCollection->parentIds.isEmpty()
                            ? CollectionId::invalid()
                            : activeCollection->parentIds.first();
                    }
                    destinationId = activeParentId;
                    const DocumentCollection *parentCollection =
                        document.collection(destinationId);
                    activeParentId = parentCollection == nullptr ||
                            parentCollection->parentIds.isEmpty()
                        ? CollectionId::invalid()
                        : parentCollection->parentIds.first();
                }
                const DocumentCollection *activeDestination =
                    destinationId.isValid()
                        ? document.collection(destinationId)
                        : nullptr;
                if (activeDestination == nullptr ||
                    !activeDestination->libraryPath.isEmpty() ||
                    !collectionBelongsToActiveScene(*activeDestination)) {
                    destinationId = CollectionId::invalid();
                }
                int destinationOrder = std::numeric_limits<int>::max();
                if (!destinationId.isValid()) {
                    for (const DocumentCollection &candidate : allCollections) {
                        if (!candidate.parentIds.isEmpty() ||
                            !candidate.libraryPath.isEmpty() ||
                            targetHierarchy.contains(candidate.id.value())) {
                            continue;
                        }
                        const bool belongsToScene = sceneName.isEmpty()
                            ? candidate.sourceSceneNames.isEmpty()
                            : collectionBelongsToActiveScene(candidate);
                        const int order = candidate.rootOrder < 0
                            ? std::numeric_limits<int>::max() - 1
                            : candidate.rootOrder;
                        if (belongsToScene && order < destinationOrder) {
                            destinationId = candidate.id;
                            destinationOrder = order;
                        }
                    }
                }

                DocumentTransaction transaction = session_.beginTransaction();
                QString createdRootSourceId;
                if (!destinationId.isValid()) {
                    QSet<QString> names;
                    for (const DocumentCollection &candidate :
                         document.collections()) {
                        names.insert(candidate.name);
                    }
                    QString rootName = QStringLiteral("Collection Instances");
                    for (int suffix = 1; names.contains(rootName); ++suffix) {
                        rootName = QStringLiteral("Collection Instances.%1")
                            .arg(suffix, 3, 10, QLatin1Char('0'));
                    }
                    DocumentCollection root;
                    root.name = rootName;
                    createdRootSourceId = QStringLiteral(
                        "classiCAD-local-collection:%1").arg(
                            QUuid::createUuid().toString(QUuid::WithoutBraces));
                    root.sourceId = createdRootSourceId;
                    root.sceneMembershipExplicit = true;
                    if (!sceneName.isEmpty()) {
                        root.sourceSceneName = sceneName;
                        root.sourceSceneNames.append(sceneName);
                    }
                    const QString viewLayerKey =
                        document.settings().activeViewLayerKey;
                    if (!viewLayerKey.isEmpty()) {
                        root.viewLayerStates.append(QJsonObject{
                            {QStringLiteral("view_layer_key"), viewLayerKey},
                            {QStringLiteral("path"), QJsonArray{root.sourceId}},
                            {QStringLiteral("exclude"), false},
                            {QStringLiteral("previously_excluded"), false},
                            {QStringLiteral("hide_viewport"), false},
                            {QStringLiteral("hide_render"), false},
                            {QStringLiteral("holdout"), false},
                            {QStringLiteral("indirect_only"), false}});
                    }
                    destinationId = transaction.insertCollection(root);
                    if (!destinationId.isValid()) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not create a collection for the instance"),
                            4000);
                        return;
                    }
                }

                QSet<QString> usedObjectNames;
                for (const SceneObject &object : document.objects()) {
                    usedObjectNames.insert(object.displayName);
                }
                QVector<ObjectId> instanceIds;
                QStringList instanceSourceObjectIds;
                for (const DocumentCollection &targetCollection :
                     targetCollections) {
                    SceneObject instance;
                    instance.instanceCollectionId = targetCollection.id;
                    instance.collectionIds = {destinationId};
                    const QString baseName = targetCollection.name +
                        QStringLiteral(" Instance");
                    instance.displayName = baseName;
                    for (int suffix = 1;
                         usedObjectNames.contains(instance.displayName);
                         ++suffix) {
                        instance.displayName = QStringLiteral("%1.%2")
                            .arg(baseName)
                            .arg(suffix, 3, 10, QLatin1Char('0'));
                    }
                    usedObjectNames.insert(instance.displayName);
                    instance.blenderSourceObjectType =
                        QStringLiteral("EMPTY");
                    instance.blenderSourceObjectId = QStringLiteral(
                        "classiCAD-instance-object:%1").arg(
                            QUuid::createUuid().toString(
                                QUuid::WithoutBraces));
                    instance.geometry.geometryType = GeometryType::Point;
                    instance.geometry.points.append(QPointF(0.0, 0.0));
                    instance.geometry.workPlaneFrame =
                        makeWorkPlaneFrame(WorkPlane::XY);
                    instance.geometry.workPlaneFrame.origin =
                        sceneCursorLocation;
                    const ObjectId instanceId = transaction.insertObject(
                        document.size(), instance);
                    if (!instanceId.isValid()) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not create the collection instances"),
                            4000);
                        return;
                    }
                    instanceIds.append(instanceId);
                    instanceSourceObjectIds.append(
                        instance.blenderSourceObjectId);
                }

                if (!sceneId.isEmpty()) {
                    DocumentSettings settings = document.settings();
                    QJsonArray scenes = settings.importedBlenderScenes;
                    bool sceneFound = false;
                    for (qsizetype index = 0; index < scenes.size(); ++index) {
                        QJsonObject scene = scenes[index].toObject();
                        if (scene.value(QStringLiteral("scene_id")).toString() !=
                            sceneId) {
                            continue;
                        }
                        QJsonArray objectIds = scene.value(
                            QStringLiteral("object_ids")).toArray();
                        for (const QString &sourceObjectId :
                             instanceSourceObjectIds) {
                            if (!objectIds.contains(sourceObjectId)) {
                                objectIds.append(sourceObjectId);
                            }
                        }
                        scene.insert(QStringLiteral("object_ids"), objectIds);
                        QJsonArray collectionIds = scene.value(
                            QStringLiteral("collection_ids")).toArray();
                        if (!createdRootSourceId.isEmpty() &&
                            !collectionIds.contains(createdRootSourceId)) {
                            collectionIds.append(createdRootSourceId);
                        }
                        scene.insert(QStringLiteral("collection_ids"),
                                     collectionIds);
                        scenes[index] = scene;
                        sceneFound = true;
                        break;
                    }
                    if (!sceneFound) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not register the instance in the active scene"),
                            4000);
                        return;
                    }
                    settings.importedBlenderScenes = scenes;
                    if (!transaction.setSettings(settings)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not save the active scene instance"),
                            4000);
                        return;
                    }
                }

                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not create the collection instances"),
                        4000);
                    return;
                }
                const ObjectId activeInstanceId = instanceIds.last();
                session_.selection().setObjectIds(instanceIds,
                                                  activeInstanceId);
                session_.selection().setActiveObjectId(activeInstanceId);
                statusBar()->showMessage(
                    QStringLiteral("Created %1 collection instance(s)")
                        .arg(instanceIds.size()),
                    4000);
            };
        collectionCallbacks.hasOutlinerCollectionClipboard = [this]() {
            return !outlinerCollectionClipboardCollections_.isEmpty();
        };
        collectionCallbacks.copyOutlinerCollection =
            [this](const QVector<CollectionId> &requestedRootIds) {
                const Document &document = session_.document();
                QVector<CollectionId> rootIds;
                QSet<quint64> rootValues;
                for (const CollectionId rootId : requestedRootIds) {
                    const DocumentCollection *root =
                        document.collection(rootId);
                    if (root == nullptr || rootValues.contains(rootId.value())) {
                        continue;
                    }
                    rootValues.insert(rootId.value());
                    rootIds.append(rootId);
                }
                if (rootIds.isEmpty()) return;
                QVector<CollectionId> topLevelRootIds;
                for (const CollectionId rootId : rootIds) {
                    QVector<CollectionId> pendingParents;
                    const DocumentCollection *root =
                        document.collection(rootId);
                    if (root != nullptr) pendingParents = root->parentIds;
                    QSet<quint64> visitedParents;
                    bool hasSelectedAncestor = false;
                    while (!pendingParents.isEmpty() &&
                           !hasSelectedAncestor) {
                        const CollectionId parentId = pendingParents.takeLast();
                        if (!parentId.isValid() ||
                            visitedParents.contains(parentId.value())) {
                            continue;
                        }
                        visitedParents.insert(parentId.value());
                        if (rootValues.contains(parentId.value())) {
                            hasSelectedAncestor = true;
                            break;
                        }
                        const DocumentCollection *parent =
                            document.collection(parentId);
                        if (parent != nullptr) {
                            for (const CollectionId ancestorId :
                                 parent->parentIds) {
                                pendingParents.append(ancestorId);
                            }
                        }
                    }
                    if (!hasSelectedAncestor) {
                        topLevelRootIds.append(rootId);
                    }
                }
                rootIds = std::move(topLevelRootIds);
                rootValues.clear();
                for (const CollectionId rootId : rootIds) {
                    rootValues.insert(rootId.value());
                }
                if (rootIds.isEmpty()) return;

                outlinerObjectClipboard_.clear();
                outlinerObjectClipboardMaterials_.clear();
                outlinerObjectClipboardAnchorIdentities_.clear();
                outlinerObjectClipboardCollectionIdentities_.clear();
                outlinerCollectionClipboardRootIds_ = rootIds;
                outlinerCollectionClipboardCollections_.clear();
                outlinerCollectionClipboardObjects_.clear();
                outlinerCollectionClipboardMaterials_.clear();
                outlinerCollectionClipboardAnchorIdentities_.clear();
                outlinerCollectionClipboardCollectionIdentities_.clear();

                for (const DocumentCollection &collection :
                     document.collections()) {
                    const QString identity = collection.sourceId.isEmpty()
                        ? QStringLiteral("classiCAD-local-collection:%1")
                              .arg(collection.id.value())
                        : QStringLiteral("%1\n%2")
                              .arg(collection.libraryPath,
                                   collection.sourceId);
                    outlinerCollectionClipboardCollectionIdentities_.insert(
                        collection.id.value(), identity);
                }

                QVector<CollectionId> hierarchy = rootIds;
                QSet<quint64> hierarchyValues = rootValues;
                for (qsizetype index = 0; index < hierarchy.size(); ++index) {
                    const CollectionId parentId = hierarchy[index];
                    for (const DocumentCollection &candidate :
                         document.collections()) {
                        if (candidate.parentIds.contains(parentId) &&
                            !hierarchyValues.contains(candidate.id.value())) {
                            hierarchyValues.insert(candidate.id.value());
                            hierarchy.append(candidate.id);
                        }
                    }
                }
                for (const DocumentCollection &collection :
                     document.collections()) {
                    if (hierarchyValues.contains(collection.id.value())) {
                        outlinerCollectionClipboardCollections_.append(
                            collection);
                    }
                }

                QSet<QString> copiedObjectIdentities;
                for (const SceneObject &object : document.objects()) {
                    if (object.blenderLooseMeshEdgeCompanion) continue;
                    const bool belongsToCopiedTree = std::any_of(
                        object.collectionIds.cbegin(),
                        object.collectionIds.cend(),
                        [&hierarchyValues](CollectionId collectionId) {
                            return hierarchyValues.contains(
                                collectionId.value());
                        });
                    if (!belongsToCopiedTree) continue;
                    outlinerCollectionClipboardObjects_.append(object);
                    copiedObjectIdentities.insert(
                        object.blenderSourceObjectId.isEmpty()
                            ? QStringLiteral("classiCAD-local-object:%1")
                                  .arg(object.id.value())
                            : object.blenderSourceObjectId);
                }
                for (const SceneObject &object : document.objects()) {
                    if (object.blenderLooseMeshEdgeCompanion &&
                        copiedObjectIdentities.contains(
                            object.blenderSourceObjectId)) {
                        outlinerCollectionClipboardObjects_.append(object);
                    }
                }

                QSet<QString> materialIds;
                for (const SceneObject &object :
                     outlinerCollectionClipboardObjects_) {
                    for (const QString &materialId :
                         object.geometry.polygonMesh.materialSlotIds) {
                        materialIds.insert(materialId);
                    }
                    for (const QString &materialId :
                         object.nurbsSolidFaceMaterialIds) {
                        materialIds.insert(materialId);
                    }
                    for (const DimensionAnchorReference &anchor :
                         object.geometry.dimensionAnchors) {
                        const SceneObject *target =
                            document.object(anchor.objectId);
                        if (target == nullptr) continue;
                        const QString identity =
                            target->blenderSourceObjectId.isEmpty()
                                ? QStringLiteral("classiCAD-local-object:%1")
                                      .arg(target->id.value())
                                : target->blenderSourceObjectId;
                        outlinerCollectionClipboardAnchorIdentities_.insert(
                            anchor.objectId.value(), identity);
                    }
                }
                for (const DocumentMaterial &material : document.materials()) {
                    if (materialIds.contains(material.id)) {
                        outlinerCollectionClipboardMaterials_.append(material);
                    }
                }

                const qsizetype copiedObjectCount = std::count_if(
                    outlinerCollectionClipboardObjects_.cbegin(),
                    outlinerCollectionClipboardObjects_.cend(),
                    [](const SceneObject &object) {
                        return !object.blenderLooseMeshEdgeCompanion;
                    });
                if (!persistOutlinerClipboard()) {
                    statusBar()->showMessage(
                        QStringLiteral("Copied %1 collection(s), but could not save the Outliner clipboard")
                            .arg(rootIds.size()),
                        4000);
                } else {
                    statusBar()->showMessage(
                        QStringLiteral("Copied %1 collection(s) and %2 object(s) to the Outliner clipboard")
                            .arg(rootIds.size())
                            .arg(copiedObjectCount),
                        4000);
                }
            };
        collectionCallbacks.duplicateCollection =
            [this, activeSceneRecord, collectionBelongsToScene,
             blenderDataTypeKey,
             updateSceneCollectionIds](const QVector<CollectionId> &requestedRootIds,
                                      CollectionId contextParentId,
                                      const QHash<quint64, CollectionId>
                                          &requestedRootParentIds,
                                      bool linkedData,
                                      bool fromClipboard) {
                const Document &document = session_.document();
                const QVector<DocumentCollection> sourceCollections =
                    fromClipboard
                        ? outlinerCollectionClipboardCollections_
                        : document.collections();
                const QVector<SceneObject> sourceObjects =
                    fromClipboard ? outlinerCollectionClipboardObjects_
                                  : document.objects();
                QHash<quint64, DocumentCollection> collectionsById;
                for (const DocumentCollection &collection : sourceCollections) {
                    collectionsById.insert(collection.id.value(), collection);
                }
                QVector<CollectionId> rootIds;
                QSet<quint64> rootValues;
                for (const CollectionId rootId : requestedRootIds) {
                    if (!rootId.isValid() ||
                        !collectionsById.contains(rootId.value()) ||
                        rootValues.contains(rootId.value())) {
                        continue;
                    }
                    rootValues.insert(rootId.value());
                    rootIds.append(rootId);
                }
                if (rootIds.isEmpty()) return;
                if (fromClipboard) {
                    const DocumentCollection *destination =
                        contextParentId.isValid()
                            ? document.collection(contextParentId)
                            : nullptr;
                    if (contextParentId.isValid() &&
                        (destination == nullptr ||
                         !destination->libraryPath.isEmpty())) {
                        contextParentId = CollectionId::invalid();
                    }
                } else if (contextParentId.isValid() &&
                           !collectionsById.contains(
                               contextParentId.value())) {
                    contextParentId = CollectionId::invalid();
                }
                if (!fromClipboard && contextParentId.isValid() &&
                    !collectionsById.value(contextParentId.value())
                         .libraryPath.isEmpty()) {
                    contextParentId = CollectionId::invalid();
                }

                QVector<CollectionId> hierarchy = rootIds;
                QSet<quint64> hierarchyValues = rootValues;
                for (qsizetype index = 0; index < hierarchy.size(); ++index) {
                    const CollectionId parentId = hierarchy[index];
                    for (const DocumentCollection &candidate :
                         sourceCollections) {
                        if (candidate.parentIds.contains(parentId) &&
                            !hierarchyValues.contains(candidate.id.value())) {
                            hierarchyValues.insert(candidate.id.value());
                            hierarchy.append(candidate.id);
                        }
                    }
                }

                struct BlenderIdNames {
                    QSet<QString> fullNames;
                    QHash<QString, QSet<int>> suffixesByBase;
                };
                const auto splitNameNumber = [](const QString &value) {
                    const qsizetype separator =
                        value.lastIndexOf(QLatin1Char('.'));
                    if (separator > 0) {
                        bool parsed = false;
                        const int number = value.mid(separator + 1)
                                               .toInt(&parsed);
                        if (parsed && number >= 0) {
                            return qMakePair(value.left(separator), number);
                        }
                    }
                    return qMakePair(value, 0);
                };
                const auto recordName = [&splitNameNumber](
                    BlenderIdNames &names, const QString &name) {
                    names.fullNames.insert(name);
                    const auto parts = splitNameNumber(name);
                    names.suffixesByBase[parts.first].insert(parts.second);
                };
                const auto uniqueName = [&splitNameNumber, &recordName](
                    const QString &sourceName,
                    BlenderIdNames &usedNames,
                    const QString &fallback) {
                    const QString base = sourceName.trimmed().isEmpty()
                        ? fallback : sourceName.trimmed();
                    if (!usedNames.fullNames.contains(base)) {
                        recordName(usedNames, base);
                        return base;
                    }
                    const QString stem = splitNameNumber(base).first;
                    const QSet<int> usedSuffixes =
                        usedNames.suffixesByBase.value(stem);
                    int suffix = 1;
                    while (usedSuffixes.contains(suffix)) ++suffix;
                    const QString candidate = QStringLiteral("%1.%2")
                        .arg(stem)
                        .arg(suffix, 3, 10, QLatin1Char('0'));
                    recordName(usedNames, candidate);
                    return candidate;
                };

                BlenderIdNames collectionNames;
                for (const DocumentCollection &collection :
                     document.collections()) {
                    if (collection.libraryPath.isEmpty()) {
                        recordName(collectionNames, collection.name);
                    }
                }
                BlenderIdNames objectNames;
                QHash<QString, BlenderIdNames> dataNamesByType;
                const QString activeSceneName = activeSceneRecord().value(
                    QStringLiteral("name")).toString();
                const auto collectObjectNames =
                    [&objectNames, &dataNamesByType, &recordName,
                     blenderDataTypeKey](
                        const QVector<SceneObject> &objects) {
                        for (const SceneObject &object : objects) {
                            if (!object.libraryPath.isEmpty() &&
                                !object.blenderLooseMeshEdgeCompanion) {
                                if (!object.blenderSourceDataName.isEmpty()) {
                                    recordName(dataNamesByType[
                                                   blenderDataTypeKey(object)],
                                               object.blenderSourceDataName);
                                }
                                continue;
                            }
                            if (!object.blenderLooseMeshEdgeCompanion &&
                                !object.displayName.isEmpty()) {
                                recordName(objectNames, object.displayName);
                            }
                            if (!object.blenderSourceDataName.isEmpty()) {
                                recordName(dataNamesByType[
                                               blenderDataTypeKey(object)],
                                           object.blenderSourceDataName);
                            }
                        }
                    };
                collectObjectNames(document.objects());
                collectObjectNames(sourceObjects);

                QHash<QString, QString> materialIdRemap;
                QVector<DocumentMaterial> materialsToAdd;
                if (fromClipboard) {
                    for (DocumentMaterial material :
                         outlinerCollectionClipboardMaterials_) {
                        const QString sourceMaterialId = material.id;
                        if (sourceMaterialId.isEmpty() ||
                            materialIdRemap.contains(sourceMaterialId)) {
                            continue;
                        }
                        material.id = QStringLiteral(
                            "classiCAD-outliner-copy-material:%1")
                            .arg(QUuid::createUuid().toString(
                                QUuid::WithoutBraces));
                        material.libraryPath.clear();
                        materialIdRemap.insert(sourceMaterialId, material.id);
                        materialsToAdd.append(std::move(material));
                    }
                }

                DocumentTransaction transaction = session_.beginTransaction();
                for (const DocumentMaterial &material : materialsToAdd) {
                    if (!transaction.addMaterial(material)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not copy collection materials"),
                            4000);
                        return;
                    }
                }
                QHash<quint64, CollectionId> clonedCollectionIds;
                QVector<CollectionId> clonedRootIds;
                for (const CollectionId sourceId : hierarchy) {
                    const DocumentCollection source =
                        collectionsById.value(sourceId.value());
                    DocumentCollection clone = source;
                    clone.id = CollectionId::invalid();
                    clone.isSceneMaster = false;
                    clone.name = uniqueName(source.name, collectionNames,
                                            QStringLiteral("Collection"));
                    clone.sourceId.clear();
                    clone.sourceSceneName = activeSceneName;
                    clone.sourceSceneNames.clear();
                    if (!activeSceneName.isEmpty()) {
                        clone.sourceSceneNames.append(activeSceneName);
                    }
                    clone.sourceSceneLibraryPath.clear();
                    clone.libraryPath.clear();
                    clone.parentIds.clear();
                    clone.parentOrders.clear();
                    clone.rootOrder = -1;
                    clone.viewLayerStates = {};
                    if (rootValues.contains(sourceId.value())) {
                        if (contextParentId.isValid()) {
                            if (fromClipboard) {
                                clone.parentIds.append(contextParentId);
                            }
                        }
                    } else {
                        for (const CollectionId sourceParentId :
                             source.parentIds) {
                            const auto clonedParent =
                                clonedCollectionIds.constFind(
                                    sourceParentId.value());
                            if (clonedParent != clonedCollectionIds.cend() &&
                                !clone.parentIds.contains(clonedParent.value())) {
                                clone.parentIds.append(clonedParent.value());
                            }
                        }
                    }
                    const CollectionId cloneId =
                        transaction.insertCollection(clone);
                    if (!cloneId.isValid()) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not duplicate collection tree"),
                            4000);
                        return;
                    }
                    clonedCollectionIds.insert(sourceId.value(), cloneId);
                    if (rootValues.contains(sourceId.value())) {
                        clonedRootIds.append(cloneId);
                    }
                }
                if (!fromClipboard) {
                    for (qsizetype index = 0;
                         index < clonedRootIds.size(); ++index) {
                        const DocumentCollection sourceRoot =
                            collectionsById.value(rootIds[index].value());
                        CollectionId destinationParentId =
                            contextParentId;
                        const auto selectedParent =
                            requestedRootParentIds.constFind(
                                sourceRoot.id.value());
                        if (selectedParent !=
                            requestedRootParentIds.cend()) {
                            destinationParentId = selectedParent.value();
                        } else if (!sourceRoot.parentIds.contains(
                                       destinationParentId)) {
                            destinationParentId = rootIds.size() > 1 &&
                                    !sourceRoot.parentIds.isEmpty()
                                ? sourceRoot.parentIds.first()
                                : CollectionId::invalid();
                        }
                        transaction.moveCollectionRelative(
                            clonedRootIds[index], rootIds[index],
                            destinationParentId, true);
                    }
                }

                struct ObjectCloneSource {
                    ObjectId sourceId = ObjectId::invalid();
                    QString sourceIdentity;
                    QString sourceParentIdentity;
                    bool companion = false;
                    QString companionSourceIdentity;
                };
                QVector<SceneObject> clones;
                QVector<ObjectCloneSource> cloneSources;
                QHash<QString, int> cloneIndexBySourceIdentity;
                QHash<QString, QString> duplicatedDataNames;
                const auto objectIdentity = [](const SceneObject &object) {
                    return object.blenderSourceObjectId.isEmpty()
                        ? QStringLiteral("classiCAD-local-object:%1")
                              .arg(object.id.value())
                        : object.blenderSourceObjectId;
                };
                const auto remappedMemberships =
                    [&clonedCollectionIds](const SceneObject &object) {
                        QVector<CollectionId> result;
                        for (const CollectionId membership :
                             object.collectionIds) {
                            const auto mapped = clonedCollectionIds.constFind(
                                membership.value());
                            if (mapped != clonedCollectionIds.cend() &&
                                !result.contains(mapped.value())) {
                                result.append(mapped.value());
                            }
                        }
                        return result;
                    };
                for (const SceneObject &source : sourceObjects) {
                    if (source.blenderLooseMeshEdgeCompanion) continue;
                    const QVector<CollectionId> memberships =
                        remappedMemberships(source);
                    if (memberships.isEmpty()) continue;
                    SceneObject clone = source;
                    clone.id = ObjectId::invalid();
                    clone.collectionIds = memberships;
                    clone.libraryPath.clear();
                    clone.workbenchRandomLibraryPath.clear();
                    clone.blenderSourceObjectId.clear();
                    if (fromClipboard) {
                        clone.blenderParentObjectId.clear();
                    }
                    if (clone.instanceCollectionId.isValid()) {
                        const auto mappedInstance = clonedCollectionIds.constFind(
                            clone.instanceCollectionId.value());
                        if (mappedInstance != clonedCollectionIds.cend()) {
                            clone.instanceCollectionId = mappedInstance.value();
                        } else if (fromClipboard) {
                            const QString sourceIdentity =
                                outlinerCollectionClipboardCollectionIdentities_
                                    .value(clone.instanceCollectionId.value());
                            CollectionId currentTarget =
                                CollectionId::invalid();
                            for (const DocumentCollection &candidate :
                                 document.collections()) {
                                const QString candidateIdentity =
                                    candidate.sourceId.isEmpty()
                                        ? QStringLiteral(
                                              "classiCAD-local-collection:%1")
                                              .arg(candidate.id.value())
                                        : QStringLiteral("%1\n%2")
                                              .arg(candidate.libraryPath,
                                                   candidate.sourceId);
                                if (!sourceIdentity.isEmpty() &&
                                    candidateIdentity == sourceIdentity) {
                                    currentTarget = candidate.id;
                                    break;
                                }
                            }
                            clone.instanceCollectionId = currentTarget;
                        } else if (document.collection(
                                       clone.instanceCollectionId) == nullptr) {
                            clone.instanceCollectionId =
                                CollectionId::invalid();
                        }
                        if (!clone.instanceCollectionId.isValid()) {
                            clone.instanceTransform = identityAffineTransform3D;
                        }
                    }
                    if (fromClipboard) {
                        clone.viewLayerHidden = QJsonObject{};
                        clone.defaultViewLayerKey =
                            document.settings().activeViewLayerKey;
                    }
                    if (!linkedData &&
                        !clone.blenderSourceDataName.isEmpty()) {
                        const QString sourceDataIdentity =
                            blenderDataTypeKey(source) + QChar(0x1f) +
                            source.blenderSourceDataName;
                        const auto duplicatedDataName =
                            duplicatedDataNames.constFind(
                                sourceDataIdentity);
                        if (duplicatedDataName !=
                            duplicatedDataNames.cend()) {
                            clone.blenderSourceDataName =
                                duplicatedDataName.value();
                        } else {
                            BlenderIdNames &dataNames =
                                dataNamesByType[blenderDataTypeKey(clone)];
                            clone.blenderSourceDataName = uniqueName(
                                clone.blenderSourceDataName, dataNames,
                                QStringLiteral("Data"));
                            duplicatedDataNames.insert(
                                sourceDataIdentity,
                                clone.blenderSourceDataName);
                        }
                    }
                    if (!clone.displayName.isEmpty()) {
                        clone.displayName = uniqueName(
                            clone.displayName, objectNames,
                            QStringLiteral("Object"));
                    }
                    if (fromClipboard) {
                        for (QString &materialId :
                             clone.geometry.polygonMesh.materialSlotIds) {
                            materialId = materialIdRemap.value(materialId);
                        }
                        for (auto materialIt =
                                 clone.nurbsSolidFaceMaterialIds.begin();
                             materialIt !=
                                 clone.nurbsSolidFaceMaterialIds.end();
                             ++materialIt) {
                            materialIt.value() = materialIdRemap.value(
                                materialIt.value());
                        }
                    }
                    const int cloneIndex = clones.size();
                    cloneIndexBySourceIdentity.insert(objectIdentity(source),
                                                      cloneIndex);
                    cloneSources.append({source.id, objectIdentity(source),
                                         source.blenderParentObjectId, false,
                                         {}});
                    clones.append(std::move(clone));
                }
                for (const SceneObject &source : sourceObjects) {
                    if (!source.blenderLooseMeshEdgeCompanion) continue;
                    const auto sourceCloneIndex = cloneIndexBySourceIdentity
                        .constFind(source.blenderSourceObjectId);
                    if (sourceCloneIndex == cloneIndexBySourceIdentity.cend()) {
                        continue;
                    }
                    SceneObject clone = source;
                    clone.id = ObjectId::invalid();
                    clone.collectionIds =
                        clones[sourceCloneIndex.value()].collectionIds;
                    clone.libraryPath.clear();
                    clone.workbenchRandomLibraryPath.clear();
                    clone.blenderSourceObjectId.clear();
                    cloneSources.append({source.id, {},
                                         source.blenderParentObjectId, true,
                                         source.blenderSourceObjectId});
                    clones.append(std::move(clone));
                }

                quint64 nextWeldGroup = 1;
                for (const SceneObject &object : sourceObjects) {
                    for (const quint64 group :
                         object.geometry.controlPointWeldGroups) {
                        if (group == std::numeric_limits<quint64>::max()) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not duplicate object geometry"),
                                4000);
                            return;
                        }
                        nextWeldGroup = qMax(nextWeldGroup, group + 1);
                    }
                }
                QHash<quint64, quint64> clonedWeldGroups;
                for (SceneObject &clone : clones) {
                    for (quint64 &group :
                         clone.geometry.controlPointWeldGroups) {
                        if (group == 0) continue;
                        if (!clonedWeldGroups.contains(group)) {
                            if (nextWeldGroup ==
                                std::numeric_limits<quint64>::max()) {
                                statusBar()->showMessage(
                                    QStringLiteral("Could not duplicate object geometry"),
                                    4000);
                                return;
                            }
                            clonedWeldGroups.insert(group, nextWeldGroup++);
                        }
                        group = clonedWeldGroups.value(group);
                    }
                }

                QVector<ObjectId> clonedObjectIds;
                if (!clones.isEmpty()) {
                    clonedObjectIds = transaction.insertObjects(
                        document.size(), clones);
                    if (clonedObjectIds.size() != clones.size()) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not duplicate collection objects"),
                            4000);
                        return;
                    }
                }
                QHash<QString, ObjectId> clonedObjectsByIdentity;
                QHash<quint64, ObjectId> clonedObjectsBySourceId;
                for (int index = 0; index < cloneSources.size(); ++index) {
                    const ObjectCloneSource &source = cloneSources[index];
                    const ObjectId cloneId = clonedObjectIds[index];
                    if (!source.companion) {
                        clonedObjectsByIdentity.insert(source.sourceIdentity,
                                                       cloneId);
                        clonedObjectsBySourceId.insert(source.sourceId.value(),
                                                       cloneId);
                    }
                }
                for (int index = 0; index < cloneSources.size(); ++index) {
                    const ObjectCloneSource &source = cloneSources[index];
                    const ObjectId cloneId = clonedObjectIds[index];
                    if (source.companion) {
                        const ObjectId sourceCloneId =
                            clonedObjectsByIdentity.value(
                                source.companionSourceIdentity,
                                ObjectId::invalid());
                        if (sourceCloneId.isValid()) {
                            const QString sourceCloneIdentity =
                                QStringLiteral("classiCAD-local-object:%1")
                                    .arg(sourceCloneId.value());
                            if (!transaction.setObjectSourceIdentity(
                                    cloneId, sourceCloneIdentity)) {
                                statusBar()->showMessage(
                                    QStringLiteral("Could not link duplicate display geometry"),
                                    4000);
                                return;
                            }
                        }
                    }
                    const ObjectId parentCloneId =
                        clonedObjectsByIdentity.value(
                            source.sourceParentIdentity,
                            ObjectId::invalid());
                    ObjectId parentId = parentCloneId;
                    if (!parentId.isValid() && fromClipboard &&
                        !source.sourceParentIdentity.isEmpty()) {
                        for (const SceneObject &candidate :
                             document.objects()) {
                            if (candidate.blenderLooseMeshEdgeCompanion) {
                                continue;
                            }
                            if (objectIdentity(candidate) ==
                                source.sourceParentIdentity) {
                                parentId = candidate.id;
                                break;
                            }
                        }
                    }
                    if (parentId.isValid() &&
                        !transaction.setObjectParent(cloneId, parentId)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not duplicate object hierarchy"),
                            4000);
                        return;
                    }
                    const Shape *currentGeometry =
                        document.shape(cloneId);
                    const bool hasAnchors = currentGeometry != nullptr &&
                        !currentGeometry->dimensionAnchors.isEmpty();
                    if (!hasAnchors) continue;
                    Shape *geometry = transaction.editGeometry(cloneId);
                    if (geometry == nullptr) continue;
                    for (DimensionAnchorReference &anchor :
                         geometry->dimensionAnchors) {
                        ObjectId targetId = ObjectId::invalid();
                        if (fromClipboard) {
                            const QString identity =
                                outlinerCollectionClipboardAnchorIdentities_
                                    .value(anchor.objectId.value());
                            targetId = clonedObjectsByIdentity.value(
                                identity, ObjectId::invalid());
                            if (!targetId.isValid() && !identity.isEmpty()) {
                                for (const SceneObject &candidate :
                                     document.objects()) {
                                    if (objectIdentity(candidate) == identity) {
                                        targetId = candidate.id;
                                        break;
                                    }
                                }
                            }
                        } else {
                            const auto mapped =
                                clonedObjectsBySourceId.constFind(
                                    anchor.objectId.value());
                            if (mapped != clonedObjectsBySourceId.cend()) {
                                targetId = mapped.value();
                            }
                        }
                        if (targetId.isValid()) {
                            anchor.objectId = targetId;
                        } else {
                            anchor = DimensionAnchorReference{};
                        }
                    }
                }

                const QJsonObject activeSceneForCopy = activeSceneRecord();
                const QString activeSceneIdForCopy = activeSceneForCopy.value(
                    QStringLiteral("scene_id")).toString();
                if (!activeSceneIdForCopy.isEmpty() &&
                    !activeSceneName.isEmpty()) {
                    QStringList sourceCollectionIds;
                    for (const DocumentCollection &collection :
                         document.collections()) {
                        if (collectionBelongsToScene(collection,
                                                     activeSceneName) &&
                            !collection.sourceId.isEmpty()) {
                            sourceCollectionIds.append(collection.sourceId);
                        }
                    }
                    if (!updateSceneCollectionIds(transaction,
                                                  activeSceneIdForCopy,
                                                  sourceCollectionIds)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not update duplicated scene collections"),
                            4000);
                        return;
                    }
                }
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not duplicate collection"), 4000);
                    return;
                }
                QVector<ObjectId> selectedCloneIds;
                for (int index = 0; index < cloneSources.size(); ++index) {
                    if (!cloneSources[index].companion) {
                        selectedCloneIds.append(clonedObjectIds[index]);
                    }
                }
                if (!selectedCloneIds.isEmpty()) {
                    const ObjectId active = selectedCloneIds.back();
                    session_.selection().setObjectIds(selectedCloneIds, active);
                    session_.selection().setActiveObjectId(active);
                }
                pruneOutlinerSelection();
                if (linkedData) {
                    statusBar()->showMessage(
                        QStringLiteral("Duplicate Linked created; shared object-data editing is not represented"),
                        6000);
                } else if (fromClipboard) {
                    statusBar()->showMessage(
                        QStringLiteral("Pasted %1 collection(s)")
                            .arg(rootIds.size()),
                        4000);
                }
            };
        const auto duplicateCollectionForPaste =
            collectionCallbacks.duplicateCollection;
        collectionCallbacks.pasteOutlinerCollection =
            [this, duplicateCollectionForPaste](CollectionId parentId) {
                if (outlinerCollectionClipboardCollections_.isEmpty() ||
                    outlinerCollectionClipboardRootIds_.isEmpty()) {
                    return;
                }
                duplicateCollectionForPaste(
                    outlinerCollectionClipboardRootIds_, parentId, {}, false,
                    true);
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
        collectionCallbacks.setCollectionColorTag =
            [this](const QVector<CollectionId> &collectionIds,
                   const QString &colorTag) {
                if (!isValidDocumentCollectionColorTag(colorTag)) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                int skipped = 0;
                QSet<quint64> seen;
                for (const CollectionId id : collectionIds) {
                    if (!id.isValid() || seen.contains(id.value())) continue;
                    seen.insert(id.value());
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection == nullptr) continue;
                    if (collection->isSceneMaster ||
                        !collection->libraryPath.isEmpty()) {
                        ++skipped;
                        continue;
                    }
                    if (collection->colorTag == colorTag) continue;
                    if (transaction.setCollectionColorTag(id, colorTag)) {
                        changed = true;
                    }
                }
                if (!changed) {
                    if (skipped > 0) {
                        statusBar()->showMessage(
                            QStringLiteral("Cannot set color tags on linked or master collections"),
                            4000);
                    }
                    return;
                }
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not set collection color tags"),
                        4000);
                    return;
                }
                if (skipped > 0) {
                    statusBar()->showMessage(
                        QStringLiteral("Updated collection color tags; skipped %1 linked or master collection(s)")
                            .arg(skipped),
                        4000);
                }
            };
        collectionCallbacks.setCollectionsAssetMarked =
            [this](const QVector<CollectionId> &ids, bool assetMarked) {
                DocumentTransaction transaction = session_.beginTransaction();
                QSet<quint64> seenIds;
                bool changed = false;
                for (const CollectionId id : ids) {
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection == nullptr ||
                        seenIds.contains(id.value())) {
                        continue;
                    }
                    seenIds.insert(id.value());
                    if (collection->isSceneMaster ||
                        !collection->libraryPath.isEmpty() ||
                        collection->assetMarked == assetMarked) {
                        continue;
                    }
                    changed = transaction.setCollectionAssetMarked(
                                  id, assetMarked) || changed;
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not change collection asset marks"),
                        4000);
                }
            };
        collectionCallbacks.makeCollectionsLocal =
            [this](const QVector<CollectionId> &ids) {
                DocumentTransaction transaction = session_.beginTransaction();
                QSet<quint64> seenIds;
                bool changed = false;
                for (const CollectionId id : ids) {
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection == nullptr ||
                        seenIds.contains(id.value())) {
                        continue;
                    }
                    seenIds.insert(id.value());
                    if (collection->libraryPath.isEmpty()) continue;
                    changed = transaction.makeCollectionLocal(id) || changed;
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not make collections local"),
                        4000);
                }
            };
        collectionCallbacks.removeCollection =
            [this, updateAllSceneCollectionIds](
                const QVector<CollectionId> &requestedIds) {
                QVector<CollectionId> collectionIds;
                QSet<quint64> seenIds;
                int skippedLinked = 0;
                for (const CollectionId id : requestedIds) {
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection == nullptr || seenIds.contains(id.value())) {
                        continue;
                    }
                    seenIds.insert(id.value());
                    if (collection->isSceneMaster ||
                        !collection->libraryPath.isEmpty()) {
                        ++skippedLinked;
                        continue;
                    }
                    collectionIds.append(id);
                }
                if (collectionIds.isEmpty()) {
                    statusBar()->showMessage(
                        skippedLinked > 0
                            ? QStringLiteral("Cannot delete linked or master collections")
                            : QStringLiteral("No collections to delete"),
                        4000);
                    return;
                }

                DocumentTransaction transaction = session_.beginTransaction();
                for (const CollectionId id : collectionIds) {
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection == nullptr) continue;
                    const QVector<CollectionId> parentIds =
                        collection->parentIds;
                    QVector<CollectionId> childCollectionIds;
                    for (const DocumentCollection &candidate :
                         session_.document().collections()) {
                        if (candidate.parentIds.contains(id)) {
                            childCollectionIds.append(candidate.id);
                        }
                    }
                    QVector<ObjectId> linkedObjectIds;
                    for (const SceneObject &object :
                         session_.document().objects()) {
                        if (object.collectionIds.contains(id)) {
                            linkedObjectIds.append(object.id);
                        }
                    }
                    for (const CollectionId childId : childCollectionIds) {
                        const DocumentCollection *child =
                            session_.document().collection(childId);
                        if (child == nullptr) continue;
                        QVector<CollectionId> parents = child->parentIds;
                        parents.removeAll(id);
                        for (const CollectionId parentId : parentIds) {
                            if (!parents.contains(parentId)) {
                                parents.append(parentId);
                            }
                        }
                        if (!transaction.setCollectionParents(childId,
                                                              parents)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not preserve child collections"),
                                4000);
                            return;
                        }
                    }
                    for (const ObjectId objectId : linkedObjectIds) {
                        const SceneObject *object =
                            session_.document().object(objectId);
                        if (object == nullptr) continue;
                        QVector<CollectionId> memberships =
                            object->collectionIds;
                        memberships.removeAll(id);
                        for (const CollectionId parentId : parentIds) {
                            if (!memberships.contains(parentId)) {
                                memberships.append(parentId);
                            }
                        }
                        if (!transaction.setObjectCollections(objectId,
                                                              memberships)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not preserve collection objects"),
                                4000);
                            return;
                        }
                    }
                    if (!transaction.removeCollection(id)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not remove collection"),
                            4000);
                        return;
                    }
                }
                if (!updateAllSceneCollectionIds(transaction) ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not remove collections"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
                if (skippedLinked > 0) {
                    statusBar()->showMessage(
                        QStringLiteral("Deleted %1 collection(s); skipped %2 linked collection(s)")
                            .arg(collectionIds.size())
                            .arg(skippedLinked),
                        5000);
                }
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
                   bool parentObjects,
                   OutlinerDropPosition position) {
                if (sources.isEmpty()) return;
                if (sources.first().kind == OutlinerRowKind::DataStackEntry ||
                    sources.first().kind == OutlinerRowKind::DataStackGroup) {
                    const OutlinerRowReference &source = sources.first();
                    if (sources.size() != 1 || !source.objectId.isValid()) {
                        return;
                    }

                    const bool sourceIsEntry =
                        source.kind == OutlinerRowKind::DataStackEntry;
                    const bool reorderWithinStack = sourceIsEntry &&
                        target.kind == OutlinerRowKind::DataStackEntry &&
                        source.objectId == target.objectId &&
                        source.dataStackKind == target.dataStackKind;
                    if (reorderWithinStack) {
                        qsizetype destinationIndex = target.dataStackIndex +
                            (position == OutlinerDropPosition::After ? 1 : 0);
                        if (source.dataStackIndex < destinationIndex) {
                            --destinationIndex;
                        }
                        DocumentTransaction stackTransaction =
                            session_.beginTransaction();
                        if (!stackTransaction.moveBlenderSourceDataStackEntry(
                                source.objectId, source.dataStackKind,
                                source.dataStackIndex, destinationIndex)) {
                            return;
                        }
                        if (!session_.commitTransaction(stackTransaction)) {
                            statusBar()->showMessage(
                                QStringLiteral("Could not reorder Blender data stack entry"),
                                4000);
                        }
                        return;
                    }

                    const bool targetIsObject =
                        target.kind == OutlinerRowKind::Object;
                    const bool targetIsStackRow =
                        target.kind == OutlinerRowKind::DataStackGroup ||
                        target.kind == OutlinerRowKind::DataStackEntry;
                    const bool targetStackMatches = targetIsStackRow &&
                        target.dataStackKind == source.dataStackKind;
                    if ((!targetIsObject && !targetStackMatches) ||
                        (!sourceIsEntry &&
                         target.kind == OutlinerRowKind::DataStackEntry) ||
                        !target.objectId.isValid() ||
                        target.objectId == source.objectId) {
                        return;
                    }
                    DocumentTransaction stackTransaction =
                        session_.beginTransaction();
                    if (!stackTransaction.transferBlenderSourceDataStack(
                            source.objectId, target.objectId,
                            source.dataStackKind, source.dataStackIndex,
                            !sourceIsEntry)) {
                        return;
                    }
                    if (!session_.commitTransaction(stackTransaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not copy Blender data stack data"),
                            4000);
                    }
                    return;
                }
                if (sources.first().kind == OutlinerRowKind::Material) {
                    if (target.kind != OutlinerRowKind::Object ||
                        !target.objectId.isValid()) {
                        return;
                    }
                    DocumentTransaction materialTransaction =
                        session_.beginTransaction();
                    QSet<QString> assignedMaterialIds;
                    bool materialChanged = false;
                    for (const OutlinerRowReference &source : sources) {
                        if (source.kind != OutlinerRowKind::Material ||
                            source.materialId.isEmpty() ||
                            assignedMaterialIds.contains(source.materialId)) {
                            continue;
                        }
                        assignedMaterialIds.insert(source.materialId);
                        materialChanged =
                            materialTransaction.appendMeshMaterialSlot(
                                target.objectId, source.materialId) ||
                            materialChanged;
                    }
                    if (materialChanged &&
                        !session_.commitTransaction(materialTransaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not assign material to object"),
                            4000);
                    }
                    return;
                }
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                const bool relativeCollectionDrop =
                    target.kind == OutlinerRowKind::Collection &&
                    position != OutlinerDropPosition::Into;
                CollectionId destinationCollection = relativeCollectionDrop
                    ? target.parentCollectionId
                    : (target.kind == OutlinerRowKind::Collection
                           ? target.collectionId : CollectionId::invalid());
                const bool sceneDropTarget =
                    target.kind == OutlinerRowKind::SceneRoot &&
                    !target.sceneKey.isEmpty();
                QString destinationSceneName;
                QSet<QString> destinationSceneObjectIds;
                if (sceneDropTarget) {
                    const DocumentSettings &settings =
                        session_.document().settings();
                    for (const QJsonValue &sceneValue :
                         settings.importedBlenderScenes) {
                        const QJsonObject scene = sceneValue.toObject();
                        if (scene.value(QStringLiteral("scene_id")).toString() !=
                            target.sceneKey) {
                            continue;
                        }
                        destinationSceneName = scene.value(
                            QStringLiteral("name")).toString();
                        for (const QJsonValue &objectIdValue : scene.value(
                                 QStringLiteral("object_ids")).toArray()) {
                            if (objectIdValue.isString()) {
                                destinationSceneObjectIds.insert(
                                    objectIdValue.toString());
                            }
                        }
                        break;
                    }
                    if (destinationSceneName.isEmpty()) return;
                    const auto masterCollection = std::find_if(
                        session_.document().collections().cbegin(),
                        session_.document().collections().cend(),
                        [&destinationSceneName](
                            const DocumentCollection &collection) {
                            return collection.parentIds.isEmpty() &&
                                collection.sourceSceneNames.contains(
                                    destinationSceneName);
                        });
                    if (masterCollection ==
                        session_.document().collections().cend()) {
                        return;
                    }
                    destinationCollection = masterCollection->id;
                }
                if (!relativeCollectionDrop &&
                    target.kind == OutlinerRowKind::Object) {
                    destinationCollection = target.parentCollectionId;
                }
                if (!parentObjects && destinationCollection.isValid()) {
                    const DocumentCollection *destination =
                        session_.document().collection(destinationCollection);
                    if (destination == nullptr ||
                        !destination->libraryPath.isEmpty()) {
                        return;
                    }
                }
                const ObjectId destinationObject =
                    target.kind == OutlinerRowKind::Object
                        ? target.objectId : ObjectId::invalid();
                if (parentObjects && destinationObject.isValid() &&
                    !target.sceneKey.isEmpty()) {
                    for (const OutlinerRowReference &source : sources) {
                        if (source.kind != OutlinerRowKind::Object ||
                            !source.objectId.isValid()) {
                            continue;
                        }
                        const bool sourceBelongsToTargetScene =
                            source.sceneMembershipKeys.contains(
                                target.sceneKey) ||
                            (source.sceneMembershipKeys.isEmpty() &&
                             source.sceneKey == target.sceneKey);
                        if (!sourceBelongsToTargetScene) return;
                    }
                }
                QSet<quint64> handledObjects;
                QSet<quint64> handledCollections;
                QVector<OutlinerRowReference> orderedSources = sources;
                if (position == OutlinerDropPosition::After) {
                    std::reverse(orderedSources.begin(), orderedSources.end());
                }
                for (const OutlinerRowReference &source : orderedSources) {
                    if (sceneDropTarget &&
                        source.kind != OutlinerRowKind::Object) {
                        continue;
                    }
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
                        const DocumentCollection *sourceParent =
                            source.parentCollectionId.isValid()
                                ? session_.document().collection(
                                      source.parentCollectionId)
                                : nullptr;
                        if (!link && source.parentCollectionId.isValid() &&
                            source.parentCollectionId != destinationCollection &&
                            sourceParent != nullptr &&
                            sourceParent->libraryPath.isEmpty()) {
                            parents.removeAll(source.parentCollectionId);
                        }
                        if (destinationCollection.isValid() &&
                            !parents.contains(destinationCollection)) {
                            parents.append(destinationCollection);
                        }
                        if (parents != collection->parentIds) {
                            if (!transaction.setCollectionParents(collectionId,
                                                                 parents)) {
                                continue;
                            }
                            changed = true;
                        }
                        if (relativeCollectionDrop &&
                            collectionId != target.collectionId) {
                            changed = transaction.moveCollectionRelative(
                                          collectionId, target.collectionId,
                                          destinationCollection,
                                          position == OutlinerDropPosition::After) ||
                                      changed;
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
                    if (sceneDropTarget) {
                        bool alreadyInScene =
                            destinationSceneObjectIds.contains(
                                object->blenderSourceObjectId);
                        for (const CollectionId membershipId :
                             object->collectionIds) {
                            const DocumentCollection *membership =
                                session_.document().collection(membershipId);
                            if (membership != nullptr &&
                                (membership->id == destinationCollection ||
                                 membership->sourceSceneNames.contains(
                                     destinationSceneName))) {
                                alreadyInScene = true;
                                break;
                            }
                        }
                        if (alreadyInScene) continue;
                    }
                    if (parentObjects) {
                        if (destinationObject.isValid()) {
                            const SceneObject *parentObject =
                                session_.document().object(destinationObject);
                            QString parentIdentity;
                            if (parentObject != nullptr) {
                                parentIdentity =
                                    parentObject->blenderSourceObjectId;
                                if (parentIdentity.isEmpty()) {
                                    parentIdentity = QStringLiteral(
                                        "classiCAD-local-object:%1")
                                        .arg(destinationObject.value());
                                }
                            }
                            if (source.objectId != destinationObject &&
                                !parentIdentity.isEmpty() &&
                                object->blenderParentObjectId !=
                                    parentIdentity) {
                                changed = transaction.setObjectParent(
                                              source.objectId,
                                              destinationObject) || changed;
                            }
                        } else if (!object->blenderParentObjectId.isEmpty()) {
                            changed = transaction.setObjectParent(
                                          source.objectId,
                                          ObjectId::invalid()) || changed;
                        }
                        continue;
                    }
                    if (link && !destinationCollection.isValid()) continue;

                    QVector<CollectionId> memberships = object->collectionIds;
                    if (!link && !sceneDropTarget &&
                        source.linkedCollectionId.isValid() &&
                        !source.syntheticParentRow) {
                        const DocumentCollection *sourceCollection =
                            session_.document().collection(
                                source.linkedCollectionId);
                        if (sourceCollection != nullptr &&
                            sourceCollection->libraryPath.isEmpty()) {
                            memberships.removeAll(source.linkedCollectionId);
                        }
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
        collectionCallbacks.setObjectsAssetMarked =
            [this](const QVector<ObjectId> &ids, bool assetMarked) {
                DocumentTransaction transaction = session_.beginTransaction();
                QSet<quint64> seenIds;
                bool changed = false;
                for (const ObjectId id : ids) {
                    const SceneObject *object = session_.document().object(id);
                    if (object == nullptr || seenIds.contains(id.value())) {
                        continue;
                    }
                    seenIds.insert(id.value());
                    if (object->blenderLooseMeshEdgeCompanion ||
                        !object->libraryPath.isEmpty() ||
                        object->assetMarked == assetMarked) {
                        continue;
                    }
                    changed = transaction.setObjectAssetMarked(
                                  id, assetMarked) || changed;
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not change object asset marks"),
                        4000);
                }
            };
        collectionCallbacks.makeObjectsLocal =
            [this](const QVector<ObjectId> &ids) {
                DocumentTransaction transaction = session_.beginTransaction();
                QSet<quint64> seenIds;
                bool changed = false;
                for (const ObjectId id : ids) {
                    const SceneObject *object = session_.document().object(id);
                    if (object == nullptr || seenIds.contains(id.value())) {
                        continue;
                    }
                    seenIds.insert(id.value());
                    if (object->libraryPath.isEmpty()) continue;
                    changed = transaction.makeObjectLocal(id) || changed;
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not make objects local"), 4000);
                }
            };
        collectionCallbacks.unlinkObjects =
            [this](const QVector<OutlinerObjectCollectionLink> &targets) {
                QHash<quint64, ObjectId> objectsByValue;
                QHash<quint64, QSet<quint64>> collectionsByObject;
                const Document &document = session_.document();
                for (const OutlinerObjectCollectionLink &target : targets) {
                    const SceneObject *object = document.object(target.objectId);
                    const DocumentCollection *collection =
                        document.collection(target.collectionId);
                    if (object == nullptr || collection == nullptr ||
                        !object->collectionIds.contains(target.collectionId) ||
                        !collection->libraryPath.isEmpty()) {
                        continue;
                    }
                    objectsByValue.insert(target.objectId.value(),
                                          target.objectId);
                    collectionsByObject[target.objectId.value()].insert(
                        target.collectionId.value());
                }
                if (objectsByValue.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (auto it = objectsByValue.cbegin();
                     it != objectsByValue.cend(); ++it) {
                    const SceneObject *object = document.object(it.value());
                    if (object == nullptr) continue;
                    QVector<CollectionId> memberships = object->collectionIds;
                    for (const quint64 collectionValue :
                         collectionsByObject.value(it.key())) {
                        memberships.removeAll(
                            CollectionId::fromValue(collectionValue));
                    }
                    if (memberships != object->collectionIds) {
                        changed = transaction.setObjectCollections(
                                      it.value(), memberships) || changed;
                    }
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not unlink objects from collections"),
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
        collectionCallbacks.selectObjectHierarchy =
            [this, objectHierarchyIds](const QVector<ObjectId> &roots) {
                QVector<ObjectId> selectableIds;
                ObjectId activeRoot = ObjectId::invalid();
                for (const ObjectId root : roots) {
                    if (!session_.document().isObjectEditable(root)) continue;
                    for (const ObjectId id : objectHierarchyIds(root)) {
                        const SceneObject *object =
                            session_.document().object(id);
                        if (object != nullptr &&
                            !object->blenderLooseMeshEdgeCompanion &&
                            session_.document().isObjectEditable(id) &&
                            !selectableIds.contains(id)) {
                            selectableIds.append(id);
                        }
                    }
                    if (selectableIds.contains(root)) activeRoot = root;
                }
                if (selectableIds.isEmpty()) return;
                if (!selectableIds.contains(activeRoot)) {
                    activeRoot = selectableIds.back();
                }
                session_.selection().setObjectIds(selectableIds, activeRoot);
                session_.selection().setActiveObjectId(activeRoot);
            };
        collectionCallbacks.deleteObjectSelection =
            [this, objectHierarchyIds](const QVector<ObjectId> &contextObjectIds,
                                       bool hierarchy) {
                const Document &document = session_.document();
                const QVector<ObjectId> &roots = contextObjectIds;

                const auto identityFor = [](const SceneObject &object) {
                    return object.blenderSourceObjectId.isEmpty()
                        ? QStringLiteral("classiCAD-local-object:%1")
                              .arg(object.id.value())
                        : object.blenderSourceObjectId;
                };
                QHash<QString, const SceneObject *> objectsByIdentity;
                for (const SceneObject &object : document.objects()) {
                    if (object.blenderLooseMeshEdgeCompanion) continue;
                    objectsByIdentity.insert(identityFor(object), &object);
                }

                QSet<QString> protectedParentIdentities;
                for (const SceneObject &object : document.objects()) {
                    if (object.libraryPath.isEmpty() ||
                        object.blenderLooseMeshEdgeCompanion) {
                        continue;
                    }
                    QString parentIdentity = object.blenderParentObjectId;
                    QSet<QString> visited;
                    while (!parentIdentity.isEmpty() &&
                           !visited.contains(parentIdentity)) {
                        visited.insert(parentIdentity);
                        protectedParentIdentities.insert(parentIdentity);
                        const SceneObject *parent =
                            objectsByIdentity.value(parentIdentity, nullptr);
                        if (parent == nullptr) break;
                        parentIdentity = parent->blenderParentObjectId;
                    }
                }

                QVector<ObjectId> deleteIds;
                QSet<quint64> deleteIdValues;
                QSet<QString> deletedObjectIdentities;
                for (const ObjectId root : roots) {
                    const SceneObject *rootObject = document.object(root);
                    if (rootObject == nullptr ||
                        rootObject->blenderLooseMeshEdgeCompanion) {
                        continue;
                    }
                    QVector<ObjectId> candidates;
                    if (hierarchy) {
                        candidates = objectHierarchyIds(root);
                    } else {
                        candidates.append(root);
                        const QString rootIdentity = identityFor(*rootObject);
                        for (const SceneObject &candidate : document.objects()) {
                            if (candidate.blenderLooseMeshEdgeCompanion &&
                                candidate.blenderSourceObjectId == rootIdentity) {
                                candidates.append(candidate.id);
                            }
                        }
                    }
                    for (const ObjectId id : candidates) {
                        const SceneObject *candidate = document.object(id);
                        if (candidate == nullptr ||
                            !candidate->libraryPath.isEmpty() ||
                            (!hierarchy && id != root &&
                             !candidate->blenderLooseMeshEdgeCompanion &&
                             !document.isObjectVisible(id))) {
                            continue;
                        }
                        const QString identity = candidate->blenderLooseMeshEdgeCompanion
                            ? candidate->blenderSourceObjectId
                            : identityFor(*candidate);
                        if (protectedParentIdentities.contains(identity)) continue;
                        if (!candidate->blenderLooseMeshEdgeCompanion) {
                            deletedObjectIdentities.insert(identity);
                        }
                        if (!deleteIdValues.contains(id.value())) {
                            deleteIdValues.insert(id.value());
                            deleteIds.append(id);
                        }
                    }
                }
                if (deleteIds.isEmpty()) return;

                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const SceneObject &object : document.objects()) {
                    if (deleteIdValues.contains(object.id.value()) ||
                        !object.libraryPath.isEmpty() ||
                        !deletedObjectIdentities.contains(
                            object.blenderParentObjectId)) {
                        continue;
                    }
                    changed = transaction.setObjectParent(
                                  object.id, ObjectId::invalid()) || changed;
                }
                changed = !transaction.removeObjects(deleteIds).isEmpty() ||
                          changed;
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not delete selected objects"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        const auto setCollectionsObjectSelection =
            [this, collectionBelongsToActiveScene](
                const QVector<OutlinerCollectionContext> &targets,
                bool select) {
                const Document &document = session_.document();
                const QVector<DocumentCollection> collections =
                    document.collections();
                QHash<quint64, qsizetype> collectionIndexById;
                QHash<QString, qsizetype> collectionIndexBySourceId;
                QHash<quint64, QVector<CollectionId>> childIdsByParent;
                for (qsizetype index = 0; index < collections.size(); ++index) {
                    const DocumentCollection &collection = collections[index];
                    collectionIndexById.insert(collection.id.value(), index);
                    if (!collection.sourceId.isEmpty() &&
                        !collectionIndexBySourceId.contains(
                            collection.sourceId)) {
                        collectionIndexBySourceId.insert(
                            collection.sourceId, index);
                    }
                    for (const CollectionId parentId : collection.parentIds) {
                        childIdsByParent[parentId.value()].append(
                            collection.id);
                    }
                }
                QString viewLayerKey =
                    document.settings().activeViewLayerKey;
                for (const OutlinerCollectionContext &target : targets) {
                    if (target.viewLayerKey.isEmpty()) continue;
                    viewLayerKey = target.viewLayerKey;
                    break;
                }

                QSet<quint64> collectionIds;
                const auto appendHierarchy =
                    [&collections, &collectionIds, &collectionIndexById,
                     &childIdsByParent,
                     &collectionBelongsToActiveScene](CollectionId root) {
                        const auto rootIndex = collectionIndexById.constFind(
                            root.value());
                        if (rootIndex == collectionIndexById.cend()) {
                            return;
                        }
                        const DocumentCollection &rootCollection =
                            collections[rootIndex.value()];
                        if (!collectionBelongsToActiveScene(rootCollection)) {
                            return;
                        }
                        QVector<CollectionId> pending{root};
                        QSet<quint64> visited;
                        while (!pending.isEmpty()) {
                            const CollectionId parentId = pending.takeLast();
                            if (visited.contains(parentId.value())) continue;
                            visited.insert(parentId.value());
                            const auto parentIndex =
                                collectionIndexById.constFind(
                                    parentId.value());
                            if (parentIndex == collectionIndexById.cend()) {
                                continue;
                            }
                            const DocumentCollection &parent =
                                collections[parentIndex.value()];
                            if (parent.hideSelect ||
                                !collectionBelongsToActiveScene(parent)) {
                                continue;
                            }
                            collectionIds.insert(parentId.value());
                            for (const CollectionId childId :
                                 childIdsByParent.value(parentId.value())) {
                                if (!visited.contains(childId.value())) {
                                    pending.append(childId);
                                }
                            }
                        }
                    };

                const auto pathHasPrefix = [](
                    const QStringList &path, const QStringList &prefix) {
                    if (path.size() < prefix.size()) return false;
                    for (qsizetype index = 0; index < prefix.size(); ++index) {
                        if (path[index] != prefix[index]) return false;
                    }
                    return true;
                };
                for (const OutlinerCollectionContext &target : targets) {
                    if (!target.collectionId.isValid() ||
                        (!target.viewLayerKey.isEmpty() &&
                         target.viewLayerKey != viewLayerKey)) {
                        continue;
                    }
                    if (target.viewLayerKey.isEmpty() ||
                        target.sourcePath.isEmpty()) {
                        appendHierarchy(target.collectionId);
                        continue;
                    }

                    const qsizetype rootPathIndex =
                        target.sourcePath.size() - 1;
                    for (const DocumentCollection &candidate : collections) {
                        for (const QJsonValue &stateValue :
                             candidate.viewLayerStates) {
                            const QJsonObject state = stateValue.toObject();
                            if (state.value(QStringLiteral("view_layer_key"))
                                    .toString() != target.viewLayerKey) {
                                continue;
                            }
                            const QJsonArray storedPath = state.value(
                                QStringLiteral("path")).toArray();
                            if (storedPath.size() <
                                    target.sourcePath.size() ||
                                storedPath.isEmpty()) {
                                continue;
                            }
                            QStringList sourcePath;
                            bool validPath = true;
                            for (const QJsonValue &part : storedPath) {
                                if (!part.isString()) {
                                    validPath = false;
                                    break;
                                }
                                sourcePath.append(part.toString());
                            }
                            if (!validPath ||
                                !pathHasPrefix(sourcePath,
                                               target.sourcePath) ||
                                sourcePath.last() != candidate.sourceId) {
                                continue;
                            }

                            QVector<const DocumentCollection *> pathNodes;
                            CollectionId previousId = CollectionId::invalid();
                            bool validHierarchy = true;
                            bool blockedByHideSelect = false;
                            for (qsizetype pathIndex = 0;
                                 pathIndex < sourcePath.size(); ++pathIndex) {
                                const QString &sourceId = sourcePath[pathIndex];
                                const auto nodeIndex =
                                    collectionIndexBySourceId.constFind(
                                        sourceId);
                                if (nodeIndex ==
                                        collectionIndexBySourceId.cend() ||
                                    nodeIndex.value() < 0 ||
                                    nodeIndex.value() >= collections.size()) {
                                    validHierarchy = false;
                                    break;
                                }
                                const DocumentCollection &node =
                                    collections[nodeIndex.value()];
                                if (previousId.isValid() &&
                                    !node.parentIds.contains(previousId)) {
                                    validHierarchy = false;
                                    break;
                                }
                                pathNodes.append(&node);
                                if (pathIndex >= rootPathIndex &&
                                    node.hideSelect) {
                                    blockedByHideSelect = true;
                                }
                                previousId = node.id;
                            }
                            if (!validHierarchy || pathNodes.isEmpty() ||
                                pathNodes[rootPathIndex]->id !=
                                    target.collectionId ||
                                blockedByHideSelect ||
                                state.value(QStringLiteral("exclude"))
                                    .toBool()) {
                                continue;
                            }
                            collectionIds.insert(candidate.id.value());
                        }
                    }
                }
                if (collectionIds.isEmpty()) return;

                QVector<ObjectId> collectionObjectIds;
                for (const SceneObject &object : document.objects()) {
                    if (object.blenderLooseMeshEdgeCompanion) continue;
                    const bool belongsToCollection = std::any_of(
                        object.collectionIds.cbegin(),
                        object.collectionIds.cend(),
                        [&collectionIds](CollectionId id) {
                            return collectionIds.contains(id.value());
                        });
                    if (!belongsToCollection ||
                        (select && !document.isObjectEditable(
                                       object.id, viewLayerKey))) {
                        continue;
                    }
                    collectionObjectIds.append(object.id);
                }
                if (collectionObjectIds.isEmpty()) return;

                SelectionModel &selection = session_.selection();
                QVector<ObjectId> selectedIds = selection.objectIds();
                const QVector<ObjectId> previousIds = selectedIds;
                if (select) {
                    for (const ObjectId id : collectionObjectIds) {
                        if (!selectedIds.contains(id)) selectedIds.append(id);
                    }
                } else {
                    for (const ObjectId id : collectionObjectIds) {
                        selectedIds.removeAll(id);
                    }
                }
                if (selectedIds == previousIds) return;

                ObjectId primary = selection.primaryObjectId();
                if (!selectedIds.contains(primary)) {
                    primary = selectedIds.isEmpty()
                        ? ObjectId::invalid() : selectedIds.back();
                }
                const ObjectId active = selection.activeObjectId();
                selection.setObjectIds(selectedIds, primary);
                selection.setActiveObjectId(active);
            };
        collectionCallbacks.setCollectionsObjectSelection =
            setCollectionsObjectSelection;
        collectionCallbacks.removeCollectionHierarchy =
            [this, updateAllSceneCollectionIds](
                const QVector<CollectionId> &requestedRoots) {
                const QVector<DocumentCollection> allCollections =
                    session_.document().collections();
                QVector<CollectionId> hierarchy;
                QSet<quint64> collectionIds;
                int skippedLinkedRoots = 0;
                int selectedLocalRoots = 0;
                for (const CollectionId root : requestedRoots) {
                    const DocumentCollection *rootCollection =
                        session_.document().collection(root);
                    if (rootCollection == nullptr ||
                        collectionIds.contains(root.value())) {
                        continue;
                    }
                    collectionIds.insert(root.value());
                    if (rootCollection->isSceneMaster ||
                        !rootCollection->libraryPath.isEmpty()) {
                        ++skippedLinkedRoots;
                        collectionIds.remove(root.value());
                        continue;
                    }
                    hierarchy.append(root);
                    ++selectedLocalRoots;
                }
                if (hierarchy.isEmpty()) {
                    statusBar()->showMessage(
                        skippedLinkedRoots > 0
                            ? QStringLiteral("Cannot delete linked or master collections")
                            : QStringLiteral("No collection hierarchies to delete"),
                        4000);
                    return;
                }
                for (qsizetype index = 0; index < hierarchy.size(); ++index) {
                    const CollectionId parentId = hierarchy[index];
                    for (const DocumentCollection &candidate : allCollections) {
                        if (candidate.parentIds.contains(parentId) &&
                            !collectionIds.contains(candidate.id.value())) {
                            collectionIds.insert(candidate.id.value());
                            hierarchy.append(candidate.id);
                        }
                    }
                }
                QSet<quint64> localCollectionIds;
                for (const CollectionId id : hierarchy) {
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection != nullptr &&
                        collection->libraryPath.isEmpty()) {
                        localCollectionIds.insert(id.value());
                    }
                }
                QVector<ObjectId> affectedObjects;
                for (const SceneObject &object :
                     session_.document().objects()) {
                    const bool linkedToHierarchy = std::any_of(
                        object.collectionIds.cbegin(),
                        object.collectionIds.cend(),
                        [&localCollectionIds](CollectionId id) {
                            return localCollectionIds.contains(id.value());
                        });
                    if (linkedToHierarchy) affectedObjects.append(object.id);
                }

                const auto identityFor = [](const SceneObject &object) {
                    return object.blenderSourceObjectId.isEmpty()
                        ? QStringLiteral("classiCAD-local-object:%1")
                              .arg(object.id.value())
                        : object.blenderSourceObjectId;
                };
                QHash<QString, const SceneObject *> objectsByIdentity;
                for (const SceneObject &object :
                     session_.document().objects()) {
                    if (!object.blenderLooseMeshEdgeCompanion) {
                        objectsByIdentity.insert(identityFor(object), &object);
                    }
                }
                QSet<QString> protectedParentIdentities;
                for (const SceneObject &object :
                     session_.document().objects()) {
                    if (object.libraryPath.isEmpty() ||
                        object.blenderLooseMeshEdgeCompanion) {
                        continue;
                    }
                    QString parentIdentity = object.blenderParentObjectId;
                    QSet<QString> visited;
                    while (!parentIdentity.isEmpty() &&
                           !visited.contains(parentIdentity)) {
                        visited.insert(parentIdentity);
                        protectedParentIdentities.insert(parentIdentity);
                        const SceneObject *parent =
                            objectsByIdentity.value(parentIdentity, nullptr);
                        if (parent == nullptr) break;
                        parentIdentity = parent->blenderParentObjectId;
                    }
                }

                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const CollectionId id : hierarchy) {
                    const DocumentCollection *collection =
                        session_.document().collection(id);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty()) {
                        continue;
                    }
                    changed = transaction.removeCollection(id) || changed;
                }
                QVector<ObjectId> unlinkedObjectIds;
                QSet<QString> unlinkedObjectIdentities;
                for (const ObjectId id : affectedObjects) {
                    const SceneObject *object =
                        session_.document().object(id);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        !object->collectionIds.isEmpty()) {
                        continue;
                    }
                    const QString identity = identityFor(*object);
                    if (protectedParentIdentities.contains(identity)) continue;
                    unlinkedObjectIds.append(id);
                    unlinkedObjectIdentities.insert(identity);
                }
                for (const SceneObject &object :
                     session_.document().objects()) {
                    if (!object.blenderLooseMeshEdgeCompanion ||
                        !unlinkedObjectIdentities.contains(
                            object.blenderSourceObjectId)) {
                        continue;
                    }
                    unlinkedObjectIds.append(object.id);
                }
                for (const SceneObject &object :
                     session_.document().objects()) {
                    if (object.blenderLooseMeshEdgeCompanion ||
                        !object.libraryPath.isEmpty() ||
                        !unlinkedObjectIdentities.contains(
                            object.blenderParentObjectId)) {
                        continue;
                    }
                    changed = transaction.setObjectParent(
                                  object.id, ObjectId::invalid()) || changed;
                }
                changed = !transaction.removeObjects(unlinkedObjectIds)
                              .isEmpty() || changed;
                if (!updateAllSceneCollectionIds(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update scene collection links"),
                        4000);
                    return;
                }
                changed = !transaction.changes().isEmpty();
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not delete collection hierarchy"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
                if (skippedLinkedRoots > 0) {
                    statusBar()->showMessage(
                        QStringLiteral("Deleted %1 collection hierarchy root(s); skipped %2 linked root(s)")
                            .arg(selectedLocalRoots)
                            .arg(skippedLinkedRoots),
                        5000);
                }
            };
        const auto collectionRelationIds = [this](CollectionId root) {
                QVector<CollectionId> result;
                if (!root.isValid() ||
                    session_.document().collection(root) == nullptr) {
                    return result;
                }
                QSet<quint64> visited;
                result.append(root);
                visited.insert(root.value());
                const QVector<DocumentCollection> collections =
                    session_.document().collections();
                for (qsizetype index = 0; index < result.size(); ++index) {
                    const CollectionId parentId = result[index];
                    for (const DocumentCollection &candidate : collections) {
                        if (candidate.parentIds.contains(parentId) &&
                            !visited.contains(candidate.id.value())) {
                            visited.insert(candidate.id.value());
                            result.append(candidate.id);
                        }
                    }
                }
                QVector<CollectionId> pending{root};
                for (qsizetype index = 0; index < pending.size(); ++index) {
                    const DocumentCollection *collection =
                        session_.document().collection(pending[index]);
                    if (collection == nullptr) continue;
                    for (const CollectionId parentId : collection->parentIds) {
                        if (visited.contains(parentId.value())) continue;
                        const DocumentCollection *parent =
                            session_.document().collection(parentId);
                        if (parent == nullptr) continue;
                        visited.insert(parentId.value());
                        result.append(parentId);
                        pending.append(parentId);
                    }
                }
                return result;
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
        struct CollectionRestrictionTargets {
            QVector<CollectionId> collectionIds;
            QVector<ObjectId> objectIds;
        };
        const auto layerCollectionBranchTargets = [this](
            const OutlinerCollectionContext &target,
            bool includeObjects) {
            CollectionRestrictionTargets result;
            if (!target.hasViewLayerContext || target.viewLayerKey.isEmpty() ||
                target.sourcePath.isEmpty()) {
                return result;
            }

            const Document &document = session_.document();
            const QVector<DocumentCollection> collections =
                document.collections();
            QHash<QString, QVector<const DocumentCollection *>> bySourceId;
            QHash<quint64, QVector<const DocumentCollection *>> childrenByParent;
            for (const DocumentCollection &collection : collections) {
                if (!collection.sourceId.isEmpty()) {
                    bySourceId[collection.sourceId].append(&collection);
                }
                for (const CollectionId parentId : collection.parentIds) {
                    childrenByParent[parentId.value()].append(&collection);
                }
            }

            const auto stateForPath = [&target](
                const DocumentCollection &collection,
                const QStringList &path,
                QJsonObject *state) {
                for (const QJsonValue &value : collection.viewLayerStates) {
                    const QJsonObject candidate = value.toObject();
                    if (candidate.value(QStringLiteral("view_layer_key"))
                            .toString() != target.viewLayerKey) {
                        continue;
                    }
                    const QJsonArray storedPath = candidate.value(
                        QStringLiteral("path")).toArray();
                    if (storedPath.size() != path.size()) continue;
                    bool matches = true;
                    for (qsizetype index = 0; index < path.size(); ++index) {
                        if (!storedPath[index].isString() ||
                            storedPath[index].toString() != path[index]) {
                            matches = false;
                            break;
                        }
                    }
                    if (!matches) continue;
                    if (state != nullptr) *state = candidate;
                    return true;
                }
                return false;
            };

            CollectionId previousId = CollectionId::invalid();
            QStringList prefix;
            CollectionId rootId = CollectionId::invalid();
            for (qsizetype index = 0;
                 index < target.sourcePath.size(); ++index) {
                prefix.append(target.sourcePath[index]);
                const auto candidates = bySourceId.constFind(prefix.back());
                if (candidates == bySourceId.cend()) return result;
                const DocumentCollection *matched = nullptr;
                for (const DocumentCollection *candidate : candidates.value()) {
                    if (candidate == nullptr ||
                        (previousId.isValid() &&
                         !candidate->parentIds.contains(previousId)) ||
                        (index + 1 == target.sourcePath.size() &&
                         candidate->id != target.collectionId) ||
                        !stateForPath(*candidate, prefix, nullptr)) {
                        continue;
                    }
                    matched = candidate;
                    break;
                }
                if (matched == nullptr) return result;
                previousId = matched->id;
                rootId = matched->id;
            }

            struct LayerCollectionPath {
                CollectionId id = CollectionId::invalid();
                QStringList sourcePath;
                QSet<quint64> ancestors;
            };
            QVector<LayerCollectionPath> pending;
            pending.append({rootId, target.sourcePath, {rootId.value()}});
            QSet<QString> visitedPaths;
            QSet<quint64> visitedCollections;
            QSet<quint64> visitedObjects;
            QHash<quint64, QVector<ObjectId>> objectIdsByCollection;
            if (includeObjects) {
                for (const SceneObject &object : document.objects()) {
                    for (const CollectionId collectionId : object.collectionIds) {
                        objectIdsByCollection[collectionId.value()].append(
                            object.id);
                    }
                }
            }

            for (qsizetype index = 0; index < pending.size(); ++index) {
                const LayerCollectionPath current = pending[index];
                const QString pathKey =
                    QString::number(current.id.value()) + QLatin1Char('\x1f') +
                    current.sourcePath.join(QLatin1Char('\x1e'));
                if (visitedPaths.contains(pathKey)) continue;
                visitedPaths.insert(pathKey);

                const auto currentCandidates =
                    bySourceId.constFind(current.sourcePath.back());
                if (currentCandidates == bySourceId.cend()) continue;
                const DocumentCollection *currentCollection = nullptr;
                QJsonObject currentState;
                for (const DocumentCollection *candidate :
                     currentCandidates.value()) {
                    if (candidate != nullptr && candidate->id == current.id &&
                        stateForPath(*candidate, current.sourcePath,
                                     &currentState)) {
                        currentCollection = candidate;
                        break;
                    }
                }
                if (currentCollection == nullptr ||
                    !currentCollection->libraryPath.isEmpty()) {
                    // Blender stops this recursive branch when the Collection
                    // property cannot be edited. Linked collections are
                    // read-only in classiCAD's restriction cells as well.
                    continue;
                }

                if (!visitedCollections.contains(current.id.value())) {
                    visitedCollections.insert(current.id.value());
                    result.collectionIds.append(current.id);
                }
                // Blender updates the shared Collection ID at each LayerCollection
                // occurrence, but skips direct Object changes only when this
                // occurrence itself is excluded. Descendants are still visited.
                if (includeObjects &&
                    !currentState.value(QStringLiteral("exclude")).toBool()) {
                    for (const ObjectId objectId :
                         objectIdsByCollection.value(current.id.value())) {
                        if (visitedObjects.contains(objectId.value())) continue;
                        visitedObjects.insert(objectId.value());
                        result.objectIds.append(objectId);
                    }
                }

                for (const DocumentCollection *child :
                     childrenByParent.value(current.id.value())) {
                    if (child == nullptr ||
                        current.ancestors.contains(child->id.value()) ||
                        child->sourceId.isEmpty()) {
                        continue;
                    }
                    LayerCollectionPath childPath;
                    childPath.id = child->id;
                    childPath.sourcePath = current.sourcePath;
                    childPath.sourcePath.append(child->sourceId);
                    if (!stateForPath(*child, childPath.sourcePath, nullptr)) {
                        continue;
                    }
                    childPath.ancestors = current.ancestors;
                    childPath.ancestors.insert(child->id.value());
                    pending.append(childPath);
                }
            }
            return result;
        };
        struct LayerCollectionPathState {
            CollectionId id = CollectionId::invalid();
            QStringList sourcePath;
            bool excludedOnPath = false;
            bool editableOnPath = true;
        };
        const auto layerCollectionTreePaths = [this](
            const QString &viewLayerKey) {
            QVector<LayerCollectionPathState> result;
            if (viewLayerKey.isEmpty()) return result;

            const QVector<DocumentCollection> collections =
                session_.document().collections();
            QHash<QString, QVector<const DocumentCollection *>> bySourceId;
            for (const DocumentCollection &collection : collections) {
                if (!collection.sourceId.isEmpty()) {
                    bySourceId[collection.sourceId].append(&collection);
                }
            }
            const auto stateForPath = [viewLayerKey](
                const DocumentCollection &collection,
                const QStringList &path,
                QJsonObject *state) {
                for (const QJsonValue &value : collection.viewLayerStates) {
                    const QJsonObject candidate = value.toObject();
                    if (candidate.value(QStringLiteral("view_layer_key"))
                            .toString() != viewLayerKey) {
                        continue;
                    }
                    const QJsonArray storedPath = candidate.value(
                        QStringLiteral("path")).toArray();
                    if (storedPath.size() != path.size()) continue;
                    bool matches = true;
                    for (qsizetype index = 0; index < path.size(); ++index) {
                        if (!storedPath[index].isString() ||
                            storedPath[index].toString() != path[index]) {
                            matches = false;
                            break;
                        }
                    }
                    if (!matches) continue;
                    if (state != nullptr) *state = candidate;
                    return true;
                }
                return false;
            };

            QSet<QString> visitedPaths;
            for (const DocumentCollection &collection : collections) {
                if (collection.sourceId.isEmpty()) continue;
                for (const QJsonValue &value : collection.viewLayerStates) {
                    const QJsonObject state = value.toObject();
                    if (state.value(QStringLiteral("view_layer_key"))
                            .toString() != viewLayerKey) {
                        continue;
                    }
                    QStringList path;
                    bool validPath = true;
                    for (const QJsonValue &part : state.value(
                             QStringLiteral("path")).toArray()) {
                        if (!part.isString() || part.toString().isEmpty()) {
                            validPath = false;
                            break;
                        }
                        path.append(part.toString());
                    }
                    if (!validPath || path.isEmpty() ||
                        path.back() != collection.sourceId) {
                        continue;
                    }
                    const QString pathKey =
                        QString::number(collection.id.value()) +
                        QLatin1Char('\x1f') +
                        path.join(QLatin1Char('\x1e'));
                    if (visitedPaths.contains(pathKey)) continue;

                    CollectionId previousId = CollectionId::invalid();
                    QStringList prefix;
                    bool excludedOnPath = false;
                    bool editableOnPath = true;
                    bool resolved = true;
                    for (qsizetype index = 0; index < path.size(); ++index) {
                        prefix.append(path[index]);
                        const auto candidates = bySourceId.constFind(
                            prefix.back());
                        if (candidates == bySourceId.cend()) {
                            resolved = false;
                            break;
                        }
                        const DocumentCollection *matched = nullptr;
                        QJsonObject prefixState;
                        for (const DocumentCollection *candidate :
                             candidates.value()) {
                            if (candidate == nullptr ||
                                (previousId.isValid() &&
                                 !candidate->parentIds.contains(previousId)) ||
                                (index + 1 == path.size() &&
                                 candidate->id != collection.id) ||
                                !stateForPath(*candidate, prefix,
                                              &prefixState)) {
                                continue;
                            }
                            matched = candidate;
                            break;
                        }
                        if (matched == nullptr) {
                            resolved = false;
                            break;
                        }
                        excludedOnPath = excludedOnPath ||
                            prefixState.value(QStringLiteral("exclude"))
                                .toBool();
                        editableOnPath = editableOnPath &&
                            matched->libraryPath.isEmpty();
                        previousId = matched->id;
                    }
                    if (!resolved) continue;
                    visitedPaths.insert(pathKey);
                    result.append({collection.id, path, excludedOnPath,
                                   editableOnPath});
                }
            }
            return result;
        };
        const auto sceneCollectionTreeIds = [this,
                                              collectionBelongsToActiveScene]() {
            const QVector<DocumentCollection> collections =
                session_.document().collections();
            QHash<quint64, const DocumentCollection *> byId;
            QHash<quint64, QVector<CollectionId>> childrenByParent;
            QVector<CollectionId> roots;
            for (const DocumentCollection &collection : collections) {
                byId.insert(collection.id.value(), &collection);
                if (!collectionBelongsToActiveScene(collection)) continue;
                if (collection.isSceneMaster) {
                    roots.append(collection.id);
                }
                for (const CollectionId parentId : collection.parentIds) {
                    childrenByParent[parentId.value()].append(collection.id);
                }
            }
            if (roots.isEmpty()) {
                for (const DocumentCollection &collection : collections) {
                    if (!collectionBelongsToActiveScene(collection)) continue;
                    const bool hasActiveParent = std::any_of(
                        collection.parentIds.cbegin(),
                        collection.parentIds.cend(),
                        [&byId, &collectionBelongsToActiveScene](
                            CollectionId parentId) {
                            const auto parent = byId.constFind(
                                parentId.value());
                            return parent != byId.cend() &&
                                collectionBelongsToActiveScene(*parent.value());
                        });
                    if (!hasActiveParent) roots.append(collection.id);
                }
            }

            QVector<CollectionId> result;
            QVector<CollectionId> pending = roots;
            QSet<quint64> visited;
            for (qsizetype index = 0; index < pending.size(); ++index) {
                const CollectionId id = pending[index];
                if (visited.contains(id.value())) continue;
                visited.insert(id.value());
                const auto collection = byId.constFind(id.value());
                if (collection == byId.cend() ||
                    !collectionBelongsToActiveScene(*collection.value()) ||
                    !collection.value()->libraryPath.isEmpty()) {
                    continue;
                }
                result.append(id);
                for (const CollectionId childId :
                     childrenByParent.value(id.value())) {
                    pending.append(childId);
                }
            }
            return result;
        };
        const auto collectionIsolationWrites =
            [this, collectionRelationIds, layerCollectionBranchTargets,
             layerCollectionTreePaths,
             sceneCollectionTreeIds](
                const OutlinerCollectionContext &target,
                bool clickedHidden,
                const std::function<bool(const DocumentCollection &)> &flagValue) {
                QHash<quint64, bool> writes;
                const Document &document = session_.document();
                const DocumentCollection *root =
                    document.collection(target.collectionId);
                if (root == nullptr || !root->libraryPath.isEmpty()) {
                    return writes;
                }

                QSet<quint64> related;
                for (const CollectionId id :
                     collectionRelationIds(target.collectionId)) {
                    related.insert(id.value());
                }
                if (!target.hasViewLayerContext) {
                    bool alreadyIsolated = clickedHidden && !related.isEmpty();
                    const QVector<CollectionId> treeIds =
                        sceneCollectionTreeIds();
                    for (const CollectionId id : treeIds) {
                        const DocumentCollection *candidate =
                            document.collection(id);
                        if (candidate == nullptr || candidate->isSceneMaster ||
                            id == target.collectionId) {
                            continue;
                        }
                        const bool expectedHidden =
                            !related.contains(id.value());
                        if (flagValue(*candidate) != expectedHidden) {
                            alreadyIsolated = false;
                            break;
                        }
                    }

                    // Blender's Scenes callback passes no LayerCollection to
                    // its recursive setter. That setter returns immediately;
                    // the clicked property has already changed, and only the
                    // first parent chain is explicitly made visible.
                    writes.insert(target.collectionId.value(), clickedHidden);
                    if (!alreadyIsolated) {
                        QSet<quint64> visited{target.collectionId.value()};
                        const DocumentCollection *child = root;
                        while (child != nullptr && !child->parentIds.isEmpty()) {
                            const CollectionId parentId =
                                child->parentIds.front();
                            if (visited.contains(parentId.value())) break;
                            visited.insert(parentId.value());
                            const DocumentCollection *parent =
                                document.collection(parentId);
                            if (parent == nullptr || parent->isSceneMaster) break;
                            writes.insert(parentId.value(), false);
                            child = parent;
                        }
                    }
                    return writes;
                }

                const QVector<LayerCollectionPathState> paths =
                    layerCollectionTreePaths(target.viewLayerKey);
                if (paths.isEmpty()) return writes;
                bool alreadyIsolated = clickedHidden && !related.isEmpty();
                for (const LayerCollectionPathState &path : paths) {
                    if (!path.editableOnPath || path.excludedOnPath ||
                        path.id == target.collectionId) {
                        continue;
                    }
                    const DocumentCollection *candidate =
                        document.collection(path.id);
                    if (candidate == nullptr || candidate->isSceneMaster) {
                        continue;
                    }
                    const bool expectedHidden =
                        !related.contains(path.id.value());
                    if (flagValue(*candidate) != expectedHidden) {
                        alreadyIsolated = false;
                        break;
                    }
                }
                for (const LayerCollectionPathState &path : paths) {
                    if (path.editableOnPath) {
                        writes.insert(path.id.value(), alreadyIsolated
                            ? false : true);
                    }
                }
                if (alreadyIsolated) return writes;

                const CollectionRestrictionTargets branch =
                    layerCollectionBranchTargets(target, false);
                for (const CollectionId id : branch.collectionIds) {
                    writes.insert(id.value(), false);
                }
                QStringList prefix;
                for (qsizetype index = 0;
                     index + 1 < target.sourcePath.size(); ++index) {
                    prefix.append(target.sourcePath[index]);
                    const auto ancestor = std::find_if(
                        paths.cbegin(), paths.cend(),
                        [&prefix](const LayerCollectionPathState &candidate) {
                            return candidate.sourcePath == prefix;
                        });
                    if (ancestor != paths.cend()) {
                        writes.insert(ancestor->id.value(), false);
                    }
                }
                return writes;
            };
        collectionCallbacks.setViewportVisibility =
            [this, layerCollectionBranchTargets, collectionIsolationWrites,
             collectionBelongsToActiveScene](
                const OutlinerCollectionContext &target,
                bool hideViewport, bool recursive,
                bool isolate) {
                const CollectionId id = target.collectionId;
                if (isolate) {
                    const QHash<quint64, bool> plan = collectionIsolationWrites(
                        target, hideViewport, [](const DocumentCollection &entry) {
                            return entry.hideViewport;
                        });
                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const DocumentCollection &entry :
                         session_.document().collections()) {
                        if ((!target.hasViewLayerContext &&
                             !collectionBelongsToActiveScene(entry)) ||
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
                QVector<CollectionId> collectionIds{id};
                QVector<ObjectId> objectIds;
                if (recursive && target.hasViewLayerContext) {
                    const CollectionRestrictionTargets branchTargets =
                        layerCollectionBranchTargets(target, true);
                    collectionIds = branchTargets.collectionIds;
                    objectIds = branchTargets.objectIds;
                }
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
        collectionCallbacks.setCollectionsViewportEnabled =
            [this](const QVector<OutlinerCollectionContext> &targets,
                   bool enabled) {
                const bool hideViewport = !enabled;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                QSet<quint64> processedCollections;
                QSet<QString> processedLayerPaths;
                for (const OutlinerCollectionContext &target : targets) {
                    if (!target.collectionId.isValid()) continue;
                    const DocumentCollection *collection =
                        session_.document().collection(target.collectionId);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty()) {
                        continue;
                    }
                    if (!processedCollections.contains(
                            target.collectionId.value())) {
                        processedCollections.insert(
                            target.collectionId.value());
                        const bool hideRender = collection->hideRender;
                        if (collection->hideViewport != hideViewport) {
                            changed = transaction.setCollectionVisibility(
                                          target.collectionId, hideViewport,
                                          hideRender) || changed;
                        }
                    }
                    if (!enabled || target.viewLayerKey.isEmpty() ||
                        target.sourcePath.isEmpty()) {
                        continue;
                    }
                    const QString pathKey =
                        QString::number(target.collectionId.value()) +
                        QLatin1Char('\x1f') + target.viewLayerKey +
                        QLatin1Char('\x1f') +
                        target.sourcePath.join(QLatin1Char('\x1e'));
                    if (processedLayerPaths.contains(pathKey)) continue;
                    processedLayerPaths.insert(pathKey);
                    changed = transaction.setCollectionViewLayerState(
                                  target.collectionId, target.viewLayerKey,
                                  target.sourcePath,
                                  QStringLiteral("hide_viewport"), false) ||
                              changed;
                }
                if (!changed) return;
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update collection viewport visibility"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setCollectionsRenderVisibility =
            [this](const QVector<OutlinerCollectionContext> &targets,
                   bool hideRender) {
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                QSet<quint64> processedCollections;
                for (const OutlinerCollectionContext &target : targets) {
                    if (!target.collectionId.isValid() ||
                        processedCollections.contains(
                            target.collectionId.value())) {
                        continue;
                    }
                    processedCollections.insert(target.collectionId.value());
                    const DocumentCollection *collection =
                        session_.document().collection(target.collectionId);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty() ||
                        collection->hideRender == hideRender) {
                        continue;
                    }
                    changed = transaction.setCollectionVisibility(
                                  target.collectionId,
                                  collection->hideViewport, hideRender) ||
                              changed;
                }
                if (!changed) return;
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update collection render visibility"),
                        4000);
                    return;
                }
            };
        collectionCallbacks.setCollectionRenderVisibility =
            [this, layerCollectionBranchTargets, collectionIsolationWrites,
             collectionBelongsToActiveScene](
                const OutlinerCollectionContext &target,
                bool hideRender, bool recursive,
                bool isolate) {
                const CollectionId id = target.collectionId;
                if (isolate) {
                    const QHash<quint64, bool> plan = collectionIsolationWrites(
                        target, hideRender, [](const DocumentCollection &entry) {
                            return entry.hideRender;
                        });
                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const DocumentCollection &entry :
                         session_.document().collections()) {
                        if ((!target.hasViewLayerContext &&
                             !collectionBelongsToActiveScene(entry)) ||
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
                QVector<CollectionId> collectionIds{id};
                QVector<ObjectId> objectIds;
                if (recursive && target.hasViewLayerContext) {
                    const CollectionRestrictionTargets branchTargets =
                        layerCollectionBranchTargets(target, true);
                    collectionIds = branchTargets.collectionIds;
                    objectIds = branchTargets.objectIds;
                }
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
                if (!changed) return;
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update collection render visibility"),
                        4000);
                }
            };
        collectionCallbacks.setCollectionSelectability =
            [this, layerCollectionBranchTargets, collectionIsolationWrites,
             collectionBelongsToActiveScene](
                const OutlinerCollectionContext &target,
                bool hideSelect, bool recursive,
                bool isolate) {
                const CollectionId id = target.collectionId;
                if (isolate) {
                    const QHash<quint64, bool> plan = collectionIsolationWrites(
                        target, hideSelect, [](const DocumentCollection &entry) {
                            return entry.hideSelect;
                        });
                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const DocumentCollection &entry :
                         session_.document().collections()) {
                        if ((!target.hasViewLayerContext &&
                             !collectionBelongsToActiveScene(entry)) ||
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
                QVector<CollectionId> collectionIds{id};
                QVector<ObjectId> objectIds;
                if (recursive && target.hasViewLayerContext) {
                    const CollectionRestrictionTargets branchTargets =
                        layerCollectionBranchTargets(target, true);
                    collectionIds = branchTargets.collectionIds;
                    objectIds = branchTargets.objectIds;
                }
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
        collectionCallbacks.setObjectHiddenInViewport =
            [this, objectHierarchyIds](ObjectId id, bool hidden,
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
                        object->hiddenInViewport == hidden) {
                        continue;
                    }
                    changed = transaction.setObjectHiddenInViewport(
                                  objectId, hidden) || changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update object viewport visibility"),
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
                    if (object == nullptr ||
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
        const auto setCollectionViewLayerStates =
            [this, objectsInCollections, collectionBelongsToActiveScene,
             collectionRelationIds](
                const QVector<OutlinerCollectionContext> &requestedTargets,
                const QString &stateKey, bool value, bool recursive,
                bool isolate, bool directRootClick) {
                struct LayerCollectionPath {
                    CollectionId id = CollectionId::invalid();
                    QStringList sourcePath;
                    QSet<quint64> ancestors;
                    bool excluded = false;
                    bool targetRoot = false;
                    bool currentStateValue = false;
                };
                const auto stateForPath = [](
                    const DocumentCollection &collection,
                    const QString &layerKey, const QStringList &path) {
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
                const auto pathKey = [](CollectionId id,
                                        const QStringList &path) {
                    return QString::number(id.value()) + QLatin1Char('\x1f') +
                           path.join(QLatin1Char('\x1e'));
                };

                QVector<OutlinerCollectionContext> targets;
                QString viewLayerKey;
                QSet<QString> targetKeys;
                for (const OutlinerCollectionContext &target : requestedTargets) {
                    if (!target.collectionId.isValid() ||
                        target.viewLayerKey.isEmpty() ||
                        target.sourcePath.isEmpty()) {
                        continue;
                    }
                    if (viewLayerKey.isEmpty()) {
                        viewLayerKey = target.viewLayerKey;
                    }
                    if (target.viewLayerKey != viewLayerKey) continue;
                    const QString key = pathKey(target.collectionId,
                                                target.sourcePath);
                    if (targetKeys.contains(key)) continue;
                    targetKeys.insert(key);
                    targets.append(target);
                }
                if (targets.isEmpty()) return;

                const auto isPrefix = [](const QStringList &prefix,
                                         const QStringList &path) {
                    if (prefix.size() > path.size()) return false;
                    for (qsizetype index = 0; index < prefix.size(); ++index) {
                        if (prefix[index] != path[index]) return false;
                    }
                    return true;
                };
                const auto isInTargetBranch =
                    [&targets, &isPrefix](const QStringList &path) {
                        for (const OutlinerCollectionContext &target : targets) {
                            if (isPrefix(target.sourcePath, path) ||
                                isPrefix(path, target.sourcePath)) {
                                return true;
                            }
                        }
                        return false;
                    };
                const auto isInTargetSubtree =
                    [&targets, &isPrefix](const QStringList &path) {
                        for (const OutlinerCollectionContext &target : targets) {
                            if (isPrefix(target.sourcePath, path)) return true;
                        }
                        return false;
                    };
                const auto isTargetRootPath = [&targets](
                    const QStringList &path) {
                    return std::any_of(
                        targets.cbegin(), targets.cend(),
                        [&path](const OutlinerCollectionContext &target) {
                            return target.sourcePath == path;
                        });
                };
                const bool cascadeExclude =
                    stateKey == QStringLiteral("exclude");

                if (isolate) {
                    QVector<LayerCollectionPath> allPaths;
                    QSet<QString> allPathKeys;
                    const QVector<DocumentCollection> collections =
                        session_.document().collections();
                    for (const DocumentCollection &collection : collections) {
                        if (!collectionBelongsToActiveScene(collection)) continue;
                        for (const QJsonValue &stateValue :
                             collection.viewLayerStates) {
                            const QJsonObject state = stateValue.toObject();
                            if (state.value(QStringLiteral("view_layer_key"))
                                    .toString() != viewLayerKey) {
                                continue;
                            }
                            QStringList path;
                            bool validPath = true;
                            for (const QJsonValue &part : state.value(
                                     QStringLiteral("path")).toArray()) {
                                if (!part.isString() || part.toString().isEmpty()) {
                                    validPath = false;
                                    break;
                                }
                                path.append(part.toString());
                            }
                            if (!validPath || path.isEmpty() ||
                                path.back() != collection.sourceId) {
                                continue;
                            }
                            const QString key = pathKey(collection.id, path);
                            if (allPathKeys.contains(key)) continue;

                            CollectionId previousId = CollectionId::invalid();
                            QStringList prefix;
                            bool excludedOnPath = false;
                            bool currentStateValue = false;
                            bool resolvedPath = true;
                            for (qsizetype pathIndex = 0;
                                 pathIndex < path.size(); ++pathIndex) {
                                prefix.append(path[pathIndex]);
                                const DocumentCollection *prefixCollection =
                                    nullptr;
                                QJsonObject prefixState;
                                for (const DocumentCollection &candidate :
                                     collections) {
                                    if (candidate.sourceId != prefix.back() ||
                                        (previousId.isValid() &&
                                         !candidate.parentIds.contains(
                                             previousId)) ||
                                        (pathIndex + 1 == path.size() &&
                                         candidate.id != collection.id)) {
                                        continue;
                                    }
                                    const QJsonObject candidateState =
                                        stateForPath(candidate, viewLayerKey,
                                                     prefix);
                                    if (candidateState.isEmpty()) continue;
                                    prefixCollection = &candidate;
                                    prefixState = candidateState;
                                    break;
                                }
                                if (prefixCollection == nullptr) {
                                    resolvedPath = false;
                                    break;
                                }

                                bool nodeExcluded = prefixState.value(
                                    QStringLiteral("exclude")).toBool();
                                bool nodeStateValue = prefixState.value(
                                    stateKey).toBool();
                                if (cascadeExclude &&
                                    isInTargetSubtree(prefix)) {
                                    // Blender's checkbox RNA update runs before
                                    // the Ctrl callback. Exclude first captures
                                    // descendant states, then cascades the new
                                    // value through this clicked branch.
                                    if (value) {
                                        nodeExcluded = true;
                                        nodeStateValue = true;
                                    } else if (isTargetRootPath(prefix)) {
                                        nodeExcluded = false;
                                        nodeStateValue = false;
                                    } else {
                                        nodeExcluded = prefixState.value(
                                            QStringLiteral(
                                                "previously_excluded"))
                                                           .toBool();
                                        nodeStateValue = nodeExcluded;
                                    }
                                }
                                // Blender compares the current LayerCollection
                                // even when it is excluded, then stops before
                                // visiting that node's children. Track only
                                // excluded ancestors here so this path still
                                // participates in the comparison.
                                if (pathIndex + 1 < path.size()) {
                                    excludedOnPath = excludedOnPath ||
                                        nodeExcluded;
                                }
                                if (pathIndex + 1 == path.size()) {
                                    currentStateValue = nodeStateValue;
                                }
                                previousId = prefixCollection->id;
                            }
                            if (!resolvedPath) continue;

                            allPathKeys.insert(key);
                            LayerCollectionPath entry;
                            entry.id = collection.id;
                            entry.sourcePath = path;
                            entry.excluded = excludedOnPath;
                            entry.currentStateValue = currentStateValue;
                            allPaths.append(entry);
                        }
                    }
                    QSet<quint64> targetCollectionIds;
                    QSet<quint64> targetRelatedCollectionIds;
                    for (const OutlinerCollectionContext &target : targets) {
                        targetCollectionIds.insert(
                            target.collectionId.value());
                        for (const CollectionId relatedId :
                             collectionRelationIds(target.collectionId)) {
                            targetRelatedCollectionIds.insert(
                                relatedId.value());
                        }
                    }
                    const bool isHideState =
                        stateKey == QStringLiteral("exclude") ||
                        stateKey.startsWith(QStringLiteral("hide_"));
                    bool alreadyIsolated = value == isHideState;
                    if (alreadyIsolated) {
                        for (const LayerCollectionPath &path : allPaths) {
                            const DocumentCollection *collection =
                                session_.document().collection(path.id);
                            if (path.excluded || collection == nullptr ||
                                collection->isSceneMaster ||
                                targetCollectionIds.contains(
                                    path.id.value())) {
                                continue;
                            }
                            const bool expectedValue =
                                targetRelatedCollectionIds.contains(
                                    path.id.value())
                                    ? !isHideState : isHideState;
                            const bool currentValue = cascadeExclude
                                ? path.currentStateValue
                                : stateForPath(*collection, viewLayerKey,
                                               path.sourcePath)
                                      .value(stateKey).toBool();
                            if (currentValue != expectedValue) {
                                alreadyIsolated = false;
                                break;
                            }
                        }
                    }

                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    if (cascadeExclude && value) {
                        for (const LayerCollectionPath &path : allPaths) {
                            if (!isInTargetSubtree(path.sourcePath)) continue;
                            const DocumentCollection *collection =
                                session_.document().collection(path.id);
                            if (collection == nullptr) continue;
                            const QJsonObject current = stateForPath(
                                *collection, viewLayerKey, path.sourcePath);
                            const bool previouslyExcluded =
                                isTargetRootPath(path.sourcePath)
                                    ? false
                                    : current.value(QStringLiteral("exclude"))
                                          .toBool();
                            if (current.value(QStringLiteral(
                                    "previously_excluded")).isBool() &&
                                current.value(QStringLiteral(
                                    "previously_excluded")).toBool() ==
                                    previouslyExcluded) {
                                continue;
                            }
                            changed = transaction.setCollectionViewLayerState(
                                          path.id, viewLayerKey,
                                          path.sourcePath,
                                          QStringLiteral(
                                              "previously_excluded"),
                                          previouslyExcluded) || changed;
                        }
                    }
                    for (const LayerCollectionPath &path : allPaths) {
                        const DocumentCollection *collection =
                            session_.document().collection(path.id);
                        if (collection == nullptr || collection->isSceneMaster) {
                            continue;
                        }
                        const QJsonObject current = stateForPath(
                            *collection, viewLayerKey, path.sourcePath);
                        const bool flagValue = alreadyIsolated
                            ? false
                            : (isInTargetBranch(path.sourcePath)
                                   ? !isHideState : isHideState);
                        if (current.value(stateKey).toBool() == flagValue) continue;
                        changed = transaction.setCollectionViewLayerState(
                                      path.id, viewLayerKey, path.sourcePath,
                                      stateKey, flagValue) || changed;
                    }
                    if (changed && !session_.commitTransaction(transaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not isolate active view layer collections"),
                            4000);
                        return;
                    }
                    if (changed) pruneOutlinerSelection();
                    return;
                }

                QVector<LayerCollectionPath> paths;
                QSet<QString> visitedPaths;
                const QVector<DocumentCollection> collections =
                    session_.document().collections();
                for (const OutlinerCollectionContext &target : targets) {
                    const DocumentCollection *rootCollection =
                        session_.document().collection(target.collectionId);
                    if (rootCollection == nullptr) continue;
                    QVector<LayerCollectionPath> branchPaths;
                    LayerCollectionPath rootPath;
                    rootPath.id = target.collectionId;
                    rootPath.sourcePath = target.sourcePath;
                    rootPath.ancestors.insert(target.collectionId.value());
                    rootPath.targetRoot = true;
                    rootPath.excluded = stateForPath(
                        *rootCollection, viewLayerKey, target.sourcePath)
                        .value(QStringLiteral("exclude")).toBool();
                    branchPaths.append(rootPath);
                    if (recursive || cascadeExclude) {
                        for (qsizetype index = 0; index < branchPaths.size();
                             ++index) {
                            const LayerCollectionPath parent = branchPaths[index];
                            for (const DocumentCollection &candidate : collections) {
                                if (!collectionBelongsToActiveScene(candidate) ||
                                    !candidate.parentIds.contains(parent.id) ||
                                    parent.ancestors.contains(
                                        candidate.id.value())) {
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
                                branchPaths.append(child);
                            }
                        }
                    }
                    for (const LayerCollectionPath &path : branchPaths) {
                        const QString key = pathKey(path.id, path.sourcePath);
                        if (!cascadeExclude && visitedPaths.contains(key)) {
                            continue;
                        }
                        visitedPaths.insert(key);
                        paths.append(path);
                    }
                }
                if (paths.isEmpty()) return;

                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const LayerCollectionPath &path : paths) {
                    const DocumentCollection *collection =
                        session_.document().collection(path.id);
                    if (collection == nullptr) {
                        continue;
                    }
                    const QJsonObject current = stateForPath(
                        *collection, viewLayerKey, path.sourcePath);
                    const bool targetRoot = path.targetRoot;
                    bool nextValue = value;
                    if (cascadeExclude) {
                        if (value) {
                            const bool previouslyExcluded = targetRoot
                                ? false
                                : current.value(
                                      QStringLiteral("exclude")).toBool();
                            if (!current.value(QStringLiteral(
                                    "previously_excluded")).isBool() ||
                                current.value(QStringLiteral(
                                    "previously_excluded")).toBool() !=
                                    previouslyExcluded) {
                                changed = transaction.setCollectionViewLayerState(
                                              path.id, viewLayerKey,
                                              path.sourcePath,
                                              QStringLiteral(
                                                  "previously_excluded"),
                                              previouslyExcluded) || changed;
                            }
                            nextValue = true;
                        } else {
                            nextValue = targetRoot && directRootClick
                                ? false
                                : current.value(QStringLiteral(
                                      "previously_excluded")).toBool();
                        }
                    }
                    if (current.value(stateKey).toBool() == nextValue &&
                        current.value(stateKey).isBool()) {
                        continue;
                    }
                    changed = transaction.setCollectionViewLayerState(
                                  path.id, viewLayerKey, path.sourcePath,
                                  stateKey, nextValue) || changed;
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

                if (!changed) return;
                if (!session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update active view layer collection state"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setCollectionsViewLayerState =
            [setCollectionViewLayerStates](
                const QVector<OutlinerCollectionContext> &targets,
                const QString &stateKey, bool value, bool recursive,
                bool isolate) {
                setCollectionViewLayerStates(
                    targets, stateKey, value, recursive, isolate, false);
            };
        collectionCallbacks.setCollectionViewLayerState =
            [setCollectionViewLayerStates](
                CollectionId id, const QString &viewLayerKey,
                const QStringList &sourcePath, const QString &stateKey,
                bool value, bool recursive, bool isolate) {
                const OutlinerCollectionContext target{
                    id, CollectionId::invalid(), viewLayerKey, sourcePath};
                setCollectionViewLayerStates(
                    QVector<OutlinerCollectionContext>{target}, stateKey,
                    value, recursive, isolate, true);
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
    QVector<SceneObject> outlinerObjectClipboard_;
    QVector<DocumentMaterial> outlinerObjectClipboardMaterials_;
    QHash<quint64, QString> outlinerObjectClipboardAnchorIdentities_;
    QHash<quint64, QString> outlinerObjectClipboardCollectionIdentities_;
    QVector<CollectionId> outlinerCollectionClipboardRootIds_;
    QVector<DocumentCollection> outlinerCollectionClipboardCollections_;
    QVector<SceneObject> outlinerCollectionClipboardObjects_;
    QVector<DocumentMaterial> outlinerCollectionClipboardMaterials_;
    QHash<quint64, QString> outlinerCollectionClipboardAnchorIdentities_;
    QHash<quint64, QString> outlinerCollectionClipboardCollectionIdentities_;
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
    QString importedBlenderSourcePath_;
    QByteArray importedBlenderSourceSha256_;
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
