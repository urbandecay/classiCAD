/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "tool_shelf.h"

#include "ui/input_helpers.h"

#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QScrollBar>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <utility>

namespace classiCAD {
namespace {

QToolButton *makeActionButton(const QString &text,
                              const QString &toolTip,
                              QVBoxLayout *layout)
{
    auto *button = new QToolButton;
    button->setObjectName(QStringLiteral("toolButton"));
    button->setText(text);
    button->setToolTip(toolTip);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(button);
    return button;
}

} // namespace

ToolShelf::ToolShelf(ToolShelfCallbacks callbacks, QWidget *parent)
    : QScrollArea(parent)
    , callbacks_(std::move(callbacks))
{
    setObjectName(QStringLiteral("toolShelfScrollArea"));
    setFrameShape(QFrame::NoFrame);
    setFixedWidth(98);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *shelf = new QFrame;
    shelf->setObjectName(QStringLiteral("toolShelf"));
    shelf->setMinimumWidth(82);

    auto *layout = new QVBoxLayout(shelf);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->setContentsMargins(7, 10, 7, 10);
    layout->setSpacing(6);

    auto *label = new QLabel(QStringLiteral("TOOLS"));
    label->setObjectName(QStringLiteral("shelfLabel"));
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);

    auto *group = new QButtonGroup(shelf);
    group->setExclusive(true);

    addToolButton(layout, group, QStringLiteral("↖\nSelect"), ToolId::Select, true);
    QToolButton *pointButton = addToolButton(
        layout, group, QStringLiteral("•\nPoint"), ToolId::Point);
    configureMenu(ToolId::Point, pointButton);
    QToolButton *lineButton = addToolButton(
        layout, group, QStringLiteral("╱\nLine"), ToolId::Line);
    configureMenu(ToolId::Line, lineButton);
    QToolButton *arcButton = addToolButton(
        layout, group, QStringLiteral("⌒\nArc"), ToolId::Arc);
    configureMenu(ToolId::Arc, arcButton);
    QToolButton *bezierButton = addToolButton(
        layout, group, QStringLiteral("∿\nBezier"), ToolId::Bezier);
    configureMenu(ToolId::Bezier, bezierButton);
    addToolButton(layout, group, QStringLiteral("N\nNURBS"), ToolId::Nurbs);
    QToolButton *rectangleButton = addToolButton(
        layout, group, QStringLiteral("□\nRect"), ToolId::Rectangle);
    configureMenu(ToolId::Rectangle, rectangleButton);
    QToolButton *polygonButton = addToolButton(
        layout, group, QStringLiteral("⬡\nPolygon"), ToolId::PolygonCenterCorner);
    configureMenu(ToolId::PolygonCenterCorner, polygonButton);
    QToolButton *circleButton = addToolButton(
        layout, group, QStringLiteral("○\nCircle"), ToolId::Circle);
    configureMenu(ToolId::Circle, circleButton);
    QToolButton *ellipseButton = addToolButton(
        layout, group, QStringLiteral("⬭\nEllipse"), ToolId::Ellipse);
    configureMenu(ToolId::Ellipse, ellipseButton);
    addToolButton(layout, group, QStringLiteral("▧\nPicture"), ToolId::Picture);
    QToolButton *dimensionButton = addToolButton(
        layout, group, QStringLiteral("↔\nDim"), ToolId::LinearDimension);
    dimensionButton->setToolTip(QStringLiteral("Dimensions"));
    configureMenu(ToolId::LinearDimension, dimensionButton);
    QToolButton *eraseButton = addToolButton(
        layout, group, QStringLiteral("Erase"), ToolId::Erase);
    eraseButton->setIcon(makeEraserIcon());
    eraseButton->setIconSize(QSize(24, 24));
    eraseButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    QToolButton *trimButton = addToolButton(
        layout, group, QStringLiteral("Trim"), ToolId::Trim);
    trimButton->setIcon(makeTrimIcon());
    trimButton->setIconSize(QSize(24, 24));
    trimButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    QToolButton *extrudeButton = addToolButton(
        layout, group, QStringLiteral("↑\nExtrude"), ToolId::PointExtrude);
    extrudeButton->setToolTip(
        QStringLiteral("Extrude — selected points create edges; selected curves create NURBS surfaces"));
    addToolButton(layout, group, QStringLiteral("↻\nRotate"), ToolId::Rotate);
    addToolButton(layout, group, QStringLiteral("⇄\nMirror"), ToolId::Mirror);
    QToolButton *scaleButton = addToolButton(
        layout, group, QStringLiteral("⤢\nScale"), ToolId::Scale);
    configureMenu(ToolId::Scale, scaleButton);

    auto *duplicateButton = makeActionButton(
        QStringLiteral("⧉\nDuplicate"),
        QStringLiteral("Interactive Duplicate: choose a base point and placement"),
        layout);
    connect(duplicateButton, &QToolButton::clicked, this, [this]() {
        if (callbacks_.duplicateRequested) callbacks_.duplicateRequested();
    });

    layout->addSpacing(8);
    controlPointsButton_ = makeActionButton(
        QStringLiteral("CP\nPoints"),
        QStringLiteral("Control Points — C, then P to toggle"),
        layout);
    controlPointsButton_->setCheckable(true);
    connect(controlPointsButton_, &QToolButton::toggled,
            this, [this](bool visible) {
                if (callbacks_.controlPointsVisibilityChanged) {
                    callbacks_.controlPointsVisibilityChanged(visible);
                }
            });

    auto *subdivideButton = makeActionButton(
        QStringLiteral("Subdiv\nPoints"),
        QStringLiteral("Add evenly spaced subdivision points to the selected line or curve"),
        layout);
    connect(subdivideButton, &QToolButton::clicked, this, [this]() {
        if (callbacks_.subdivideRequested) callbacks_.subdivideRequested();
    });

    auto *joinButton = makeActionButton(
        QStringLiteral("Join\nSplines"),
        QStringLiteral("Join connected lines and curves into one continuous spline"),
        layout);
    connect(joinButton, &QToolButton::clicked, this, [this]() {
        if (callbacks_.joinRequested) callbacks_.joinRequested();
    });

    auto *explodeButton = makeActionButton(
        QStringLiteral("Explode\nCurves"),
        QStringLiteral("Separate selected rectangles or joined splines into individual curves"),
        layout);
    connect(explodeButton, &QToolButton::clicked, this, [this]() {
        if (callbacks_.explodeRequested) callbacks_.explodeRequested();
    });

    layout->addStretch(1);
    helpLabel_ = new QLabel;
    helpLabel_->setObjectName(QStringLiteral("toolHelp"));
    helpLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(helpLabel_);

    layout->activate();
    shelf->adjustSize();
    setWidget(shelf);
    shelf->installEventFilter(this);
    for (QWidget *widget : shelf->findChildren<QWidget *>()) {
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

QToolButton *ToolShelf::addToolButton(QVBoxLayout *layout,
                                      QButtonGroup *group,
                                      const QString &text,
                                      ToolId tool,
                                      bool checked)
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
    buttonsByTool_.insert(static_cast<int>(tool), button);
    connect(button, &QToolButton::clicked, this, [this, tool]() {
        if (callbacks_.toolRequested) callbacks_.toolRequested(tool);
    });
    return button;
}

void ToolShelf::configureMenu(ToolId tool, QToolButton *button)
{
    if (button != nullptr && callbacks_.configureToolMenu) {
        callbacks_.configureToolMenu(tool, button);
    }
}

QToolButton *ToolShelf::button(ToolId tool) const
{
    return buttonsByTool_.value(static_cast<int>(tool), nullptr);
}

QToolButton *ToolShelf::controlPointsButton() const noexcept
{
    return controlPointsButton_;
}

QVector<QToolButton *> ToolShelf::toolButtons() const
{
    return toolButtons_;
}

void ToolShelf::setHelpText(const QString &text)
{
    if (helpLabel_ != nullptr) {
        helpLabel_->setText(text);
    }
}

bool ToolShelf::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
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

} // namespace classiCAD
