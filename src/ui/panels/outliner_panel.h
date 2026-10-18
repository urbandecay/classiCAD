#pragma once

#include "ui/viewport_widget_api.h"

#include <QFrame>

#include <functional>

class QPushButton;
class QTreeWidget;

namespace classiCAD {

struct OutlinerPanelCallbacks {
    std::function<QVector<ViewportObjectInfo>()> objectInfos;
    std::function<void(bool)> setEditMode;
    std::function<void(ObjectId, bool)> selectObject;
};

class OutlinerPanel final : public QFrame {
public:
    explicit OutlinerPanel(OutlinerPanelCallbacks callbacks,
                           QWidget *parent = nullptr);

    void refresh();
    void setObjectState(bool editMode, ObjectId activeObjectId);

private:
    void createLayout();
    void updateModeButtons();

    OutlinerPanelCallbacks callbacks_;
    QTreeWidget *tree_ = nullptr;
    QPushButton *objectModeButton_ = nullptr;
    QPushButton *editModeButton_ = nullptr;
    ObjectId activeObjectId_ = ObjectId::invalid();
    bool editMode_ = false;
    bool hasSelectedObject_ = false;
    bool refreshing_ = false;
};

} // namespace classiCAD
