#pragma once

#include "core/document/layer_id.h"
#include "core/document/object_id.h"
#include "core/model.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

namespace classiCAD {

enum class ViewportCommand {
    Undo,
    Redo,
    BeginSubdivision,
    CancelSubdivision,
    ApplySubdivision,
    BeginJoin,
    Explode,
    BeginRotate,
    BeginMirror,
};

struct ViewportCommandResult {
    bool accepted = false;
    int count = 0;
};

enum class ViewportLayerCommand {
    Create,
    Remove,
    Activate,
    SetVisible,
    SetLocked,
    Rename,
    Move,
    MoveSelectedObjects,
};

struct ViewportLayerCommandRequest {
    ViewportLayerCommand command = ViewportLayerCommand::Create;
    LayerId layerId = LayerId::invalid();
    int index = -1;
    bool enabled = false;
    QString name;
};

struct ViewportLayerCommandResult {
    bool accepted = false;
    LayerId layerId = LayerId::invalid();
    int count = 0;
};

struct ViewportLayerInfo {
    LayerId id = LayerId::invalid();
    QString name;
    bool visible = true;
    bool locked = false;
    bool active = false;
    int objectCount = 0;
};

struct ViewportUiCallbacks {
    std::function<void(const QString &)> coordinateUpdate;
    std::function<void(ToolId)> commandFinished;
    std::function<void(ToolId)> toolRepeated;
    std::function<void()> historyChanged;
    std::function<void()> layersChanged;
    std::function<void(const QString &)> subdivisionStatusUpdate;
    std::function<void(const QString &)> joinStatusUpdate;
};

class ViewportWidgetApi : public QWidget {
public:
    explicit ViewportWidgetApi(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    ~ViewportWidgetApi() override = default;

    virtual void setTool(ToolId tool) = 0;
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
                              bool near) = 0;

    virtual QString subdivisionStatusText() const = 0;
    virtual QString joinStatusText() const = 0;

    virtual bool saveUpdateSession(const QString &path) const = 0;
    virtual bool restoreUpdateSession(const QString &path) = 0;

    void setUiCallbacks(const ViewportUiCallbacks &callbacks)
    {
        coordinateUpdate_ = callbacks.coordinateUpdate;
        commandFinished_ = callbacks.commandFinished;
        toolRepeated_ = callbacks.toolRepeated;
        historyChanged_ = callbacks.historyChanged;
        layersChanged_ = callbacks.layersChanged;
        subdivisionStatusUpdate_ = callbacks.subdivisionStatusUpdate;
        joinStatusUpdate_ = callbacks.joinStatusUpdate;
    }

protected:
    std::function<void(const QString &)> coordinateUpdate_;
    std::function<void(ToolId)> commandFinished_;
    std::function<void(ToolId)> toolRepeated_;
    std::function<void()> historyChanged_;
    std::function<void()> layersChanged_;
    std::function<void(const QString &)> subdivisionStatusUpdate_;
    std::function<void(const QString &)> joinStatusUpdate_;
};

ViewportWidgetApi *createViewportWidget(QWidget *parent = nullptr);

} // namespace classiCAD
