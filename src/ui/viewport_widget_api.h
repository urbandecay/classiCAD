#pragma once

#include "app/command_router.h"
#include "core/document/layer_id.h"
#include "core/document/object_id.h"
#include "core/document/document_settings.h"
#include "core/commands/layer_command.h"
#include "core/geometry/arc_mode.h"
#include "core/geometry/work_plane.h"
#include "core/tool_id.h"
#include "services/viewport/viewport_transform.h"
#include "viewport/blender_grid_appearance.h"

#include <QString>
#include <QColor>
#include <QVector>
#include <QWidget>

#include <functional>

namespace classiCAD {

struct Rhino3dmImportReport;
struct ProjectViewportCameraSettings;
class ApplicationSession;

using ViewportCommand = ApplicationCommand;
using ViewportCommandResult = ApplicationCommandResult;

using ViewportLayerCommand = LayerCommandOperation;
using ViewportLayerCommandRequest = LayerCommandRequest;
using ViewportLayerCommandResult = LayerCommandResult;

struct ViewportLayerInfo {
    LayerId id = LayerId::invalid();
    QString name;
    bool visible = true;
    bool locked = false;
    bool active = false;
    int objectCount = 0;
    QColor color = QColor(QStringLiteral("#d28b45"));
    QString lineType = QStringLiteral("Continuous");
    qreal lineWeightMm = 0.0;
    QString description;
    bool frozen = false;
    bool plotted = true;
};

struct ViewportUiCallbacks {
    std::function<void(const QString &)> coordinateUpdate;
    std::function<void(const QString &)> toolStatusUpdate;
    std::function<void(ToolId)> commandFinished;
    std::function<void(ToolId)> toolRepeated;
    std::function<void()> historyChanged;
    std::function<void()> layersChanged;
    std::function<void(const QString &)> subdivisionStatusUpdate;
    std::function<void(const QString &)> joinStatusUpdate;
    std::function<void(WorkPlane, qreal, ViewportViewPreset)> viewStateUpdate;
};

struct RotateToolPreferences {
    qreal angleSnapIncrementDegrees = 15.0;
    qreal angleSnapIncrementRadiansDegrees = 15.0;
    qreal angleSnapStrengthDegrees = 6.0;
    bool angleSnapEnabled = true;
    bool useRadians = false;
};

class ViewportWidgetApi : public QWidget {
public:
    explicit ViewportWidgetApi(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    ~ViewportWidgetApi() override = default;

    virtual void setTool(ToolId tool) = 0;
    virtual bool beginPicturePlacement(const QString &imagePath,
                                       QString *errorMessage = nullptr) = 0;
    virtual ViewportCommandResult executeCommand(ViewportCommand command,
                                                  int argument = 0) = 0;
    virtual QVector<ViewportLayerInfo> layerInfos() const = 0;
    virtual ViewportLayerCommandResult executeLayerCommand(
        const ViewportLayerCommandRequest &request) = 0;
    virtual bool canUndo() const = 0;
    virtual bool canRedo() const = 0;
    virtual void setArcMode(ArcMode mode) = 0;
    virtual ArcMode arcMode() const = 0;
    virtual void setControlPointsVisible(bool visible) = 0;
    virtual bool controlPointsVisible() const = 0;
    virtual void setSnapLabelsVisible(bool visible) = 0;
    virtual void setSmoothCurveDisplay(bool enabled) = 0;
    virtual RotateToolPreferences rotateToolPreferences() const = 0;
    virtual bool setRotateToolPreferences(
        const RotateToolPreferences &preferences) = 0;
    virtual DocumentSettings documentSettings() const = 0;
    virtual bool setDocumentSettings(const DocumentSettings &settings) = 0;
    virtual BlenderGridAppearance gridAppearance() const = 0;
    virtual void setGridAppearance(const BlenderGridAppearance &appearance) = 0;
    virtual ViewportCameraPreferences cameraPreferences() const = 0;
    virtual bool setCameraPreferences(const ViewportCameraPreferences &preferences) = 0;
    virtual ViewportNavigationPreferences navigationPreferences() const = 0;
    virtual void setNavigationPreferences(
        const ViewportNavigationPreferences &preferences) = 0;
    virtual int viewportAntiAliasingSamples() const = 0;
    virtual void setViewportAntiAliasingSamples(int samples) = 0;
    virtual void setArchitecturalDimensionFont(bool enabled) = 0;
    virtual void setPanButton(Qt::MouseButton button) = 0;
    virtual Qt::MouseButton panButton() const = 0;
    virtual void setOrthoEnabled(bool enabled) = 0;
    virtual bool orthoEnabled() const = 0;
    virtual void setOsnapEnabled(bool enabled) = 0;
    virtual bool osnapEnabled() const = 0;
    virtual void setSnapModes(bool endpoint,
                              bool midpoint,
                              bool intersection,
                              bool center,
                              bool perpendicular,
                              bool tangent,
                              bool near,
                              bool controlPoint) = 0;

    virtual QString subdivisionStatusText() const = 0;
    virtual QString joinStatusText() const = 0;

    virtual bool saveUpdateSession(const QString &path) const = 0;
    virtual bool restoreUpdateSession(const QString &path) = 0;

    virtual ProjectViewportCameraSettings projectCameraSettings() const = 0;
    virtual void prepareForDocumentReplacement() = 0;
    virtual void applyProjectCameraSettings(
        const ProjectViewportCameraSettings &settings) = 0;
    virtual void refreshAfterProjectImport() = 0;
    virtual void refreshAfterNewDocument() = 0;
    // Compatibility adapters retained for interaction regressions and older
    // callers. The document workflows are implemented by ProjectController.
    virtual bool saveVignolaDocument(const QString &path,
                                     QString *errorMessage = nullptr) const = 0;
    virtual bool loadVignolaDocument(const QString &path,
                                     QString *errorMessage = nullptr) = 0;
    virtual bool importRhino3dmDocument(const QString &path,
                                        Rhino3dmImportReport *report = nullptr,
                                        QString *errorMessage = nullptr) = 0;
    virtual void createNewDocument() = 0;
    virtual void setWorkPlane(WorkPlane plane, qreal offset = 0.0) = 0;
    virtual WorkPlane workPlane() const = 0;
    virtual qreal workPlaneOffset() const = 0;
    virtual void setViewPreset(ViewportViewPreset preset) = 0;
    virtual ViewportViewPreset viewPreset() const = 0;

    void setUiCallbacks(const ViewportUiCallbacks &callbacks)
    {
        coordinateUpdate_ = callbacks.coordinateUpdate;
        toolStatusUpdate_ = callbacks.toolStatusUpdate;
        commandFinished_ = callbacks.commandFinished;
        toolRepeated_ = callbacks.toolRepeated;
        historyChanged_ = callbacks.historyChanged;
        layersChanged_ = callbacks.layersChanged;
        subdivisionStatusUpdate_ = callbacks.subdivisionStatusUpdate;
        joinStatusUpdate_ = callbacks.joinStatusUpdate;
        viewStateUpdate_ = callbacks.viewStateUpdate;
    }

protected:
    std::function<void(const QString &)> coordinateUpdate_;
    std::function<void(const QString &)> toolStatusUpdate_;
    std::function<void(ToolId)> commandFinished_;
    std::function<void(ToolId)> toolRepeated_;
    std::function<void()> historyChanged_;
    std::function<void()> layersChanged_;
    std::function<void(const QString &)> subdivisionStatusUpdate_;
    std::function<void(const QString &)> joinStatusUpdate_;
    std::function<void(WorkPlane, qreal, ViewportViewPreset)> viewStateUpdate_;
};

ViewportWidgetApi *createViewportWidget(QWidget *parent = nullptr);
ViewportWidgetApi *createViewportWidget(ApplicationSession &session,
                                        QWidget *parent = nullptr);

} // namespace classiCAD
