/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ui/viewport_widget_api.h"

#include <QFrame>

#include <functional>

class QLineEdit;
class QPushButton;
class QTableWidget;

namespace classiCAD {

struct LayersPanelCallbacks {
    std::function<QVector<ViewportLayerInfo>()> layerInfos;
    std::function<void()> addLayer;
    std::function<void(LayerId)> removeLayer;
    std::function<void(LayerId)> renameLayer;
    std::function<void(LayerId)> activateLayer;
    std::function<void(LayerId, bool)> setVisible;
    std::function<void(LayerId, bool)> setFrozen;
    std::function<void(LayerId, bool)> setLocked;
    std::function<void(LayerId, bool)> setPlotted;
    std::function<void(LayerId)> chooseColor;
    std::function<void(LayerId)> editDescription;
    std::function<void(LayerId, const QString &)> setLineType;
    std::function<void(LayerId, qreal)> setLineWeight;
    std::function<void(LayerId, int)> moveLayer;
    std::function<void(LayerId)> moveSelectedObjects;
    std::function<void(const QVector<ViewportLayerInfo> &)> layersRefreshed;
};

// Owns layer table presentation, filtering, selection, and panel-local controls.
// All document edits are routed through callbacks supplied by the composition root.
class LayersPanel final : public QFrame {
public:
    explicit LayersPanel(LayersPanelCallbacks callbacks,
                         QWidget *parent = nullptr);

    LayerId selectedLayerId() const;
    int currentRow() const noexcept;
    void refresh();

private:
    void createLayout();
    void updateControls();
    LayerId layerIdAtRow(int row) const;
    void moveSelectedLayer(int delta);

    LayersPanelCallbacks callbacks_;
    QLineEdit *filter_ = nullptr;
    QTableWidget *table_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QPushButton *moveUpButton_ = nullptr;
    QPushButton *moveDownButton_ = nullptr;
    QPushButton *activateButton_ = nullptr;
    QPushButton *moveObjectsButton_ = nullptr;
    bool refreshing_ = false;
};

} // namespace classiCAD
