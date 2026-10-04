#include "classicad_theme.h"

namespace classiCAD {

QString classicadThemeStyleSheet()
{
    return QStringLiteral(R"(
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
            QToolBar#layerPropertiesBar {
                background: #242424;
                border: 0;
                border-bottom: 1px solid #171717;
                spacing: 5px;
                padding: 4px 8px;
            }
            QToolBar#layerPropertiesBar QComboBox {
                min-height: 22px;
                padding: 2px 5px;
                background: #303030;
                border: 1px solid #444444;
            }
            QLabel#brand {
                color: #f0a45a;
                font-size: 15px;
                font-weight: bold;
                padding-right: 8px;
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
            QSplitter#workspaceSplitter::handle {
                background: #202020;
            }
            QSplitter#workspaceSplitter::handle:horizontal {
                width: 6px;
            }
            QSplitter#workspaceSplitter::handle:horizontal:hover {
                background: #303030;
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
            QTableWidget#layerTable {
                background: #2b2b2b;
                alternate-background-color: #303030;
                border: 0;
                selection-background-color: #5a3824;
                selection-color: #ffffff;
            }
            QTableWidget#layerTable QComboBox {
                min-height: 20px;
                padding: 1px 3px;
                background: #303030;
                border: 0;
            }
            QTableWidget#layerTable::item {
                padding: 2px 3px;
            }
            QHeaderView::section {
                background: #303030;
                border: 0;
                color: #999999;
                padding: 3px;
            }
            QLineEdit#layerFilter {
                background: #222222;
                border: 1px solid #444444;
                padding: 4px;
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
            QLabel#joinFeedback {
                color: #8ed6a4;
                font-weight: 600;
                padding-left: 10px;
                padding-right: 10px;
            }
)");
}

} // namespace classiCAD
