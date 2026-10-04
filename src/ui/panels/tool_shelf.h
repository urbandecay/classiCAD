/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/tool_id.h"

#include <QHash>
#include <QScrollArea>
#include <QVector>

#include <functional>

class QLabel;
class QEvent;
class QObject;
class QToolButton;
class QVBoxLayout;
class QButtonGroup;

namespace classiCAD {

struct ToolShelfCallbacks {
    std::function<void(ToolId)> toolRequested;
    std::function<void(ToolId, QToolButton *)> configureToolMenu;
    std::function<void()> duplicateRequested;
    std::function<void(bool)> controlPointsVisibilityChanged;
    std::function<void()> subdivideRequested;
    std::function<void()> joinRequested;
    std::function<void()> explodeRequested;
};

// Owns the tool buttons, their grouping, and their local presentation state.
// Tool execution and specialized popup contents remain application actions.
class ToolShelf final : public QScrollArea {
public:
    explicit ToolShelf(ToolShelfCallbacks callbacks,
                       QWidget *parent = nullptr);

    QToolButton *button(ToolId tool) const;
    QToolButton *controlPointsButton() const noexcept;
    QVector<QToolButton *> toolButtons() const;
    void setHelpText(const QString &text);

private:
    QToolButton *addToolButton(QVBoxLayout *layout,
                               QButtonGroup *group,
                               const QString &text,
                               ToolId tool,
                               bool checked = false);
    void configureMenu(ToolId tool, QToolButton *button);

    ToolShelfCallbacks callbacks_;
    QHash<int, QToolButton *> buttonsByTool_;
    QVector<QToolButton *> toolButtons_;
    QToolButton *controlPointsButton_ = nullptr;
    QLabel *helpLabel_ = nullptr;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace classiCAD
