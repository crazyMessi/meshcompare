#include "app_theme.h"

QString meshCompareApplicationStyleSheet()
{
    return QStringLiteral(R"QSS(
QMainWindow, QWidget#workspaceRoot {
    background: #242522;
    color: #F2F0EB;
    font-size: 13px;
}

QWidget#commandBar {
    background: #292A27;
    border-bottom: 1px solid #3A3B37;
}

QLabel#brandLabel {
    color: #FAF8F3;
    font-size: 15px;
    font-weight: 600;
}

QFrame#toolbarDivider {
    background: #494A45;
    border: 0;
}

QPushButton, QToolButton {
    min-height: 30px;
    padding: 0 11px;
    color: #E5E2DC;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 7px;
}

QPushButton:hover, QToolButton:hover {
    background: #383936;
    border-color: #4A4B46;
}

QPushButton:pressed, QToolButton:pressed {
    background: #20211F;
}

QPushButton:disabled, QToolButton:disabled {
    color: #777873;
    background: transparent;
}

QPushButton#importMeshesButton {
    background: #3C64D9;
    border-color: #4B73EA;
    color: #FFFFFF;
    font-weight: 600;
}

QPushButton#importMeshesButton:hover {
    background: #4B73EA;
}

QPushButton#overlayViewButton, QPushButton#gridViewButton {
    min-width: 64px;
    background: #20211F;
    border-color: #4A4B46;
}

QPushButton#overlayViewButton:checked, QPushButton#gridViewButton:checked {
    background: #6252D2;
    border-color: #7565E3;
    color: #FFFFFF;
    font-weight: 600;
}

QPushButton#overlayViewButton:checked:disabled,
QPushButton#gridViewButton:checked:disabled {
    background: #333430;
    border-color: #484944;
    color: #777873;
}

QLabel#linkedCameraLabel {
    color: #B9C0B3;
}

QLabel#meshCountLabel, QLabel#viewModeLabel {
    color: #AAA9A3;
}

QToolButton#diagnosticsButton {
    min-width: 30px;
    max-width: 30px;
    padding: 0;
    font-size: 18px;
}

QFrame#viewportFrame, QWidget#viewportHost {
    background: #363634;
    border: 0;
}

QWidget#noticeArea {
    background: #363634;
    border: 0;
}

QFrame#noticeBanner {
    background: #292A27;
    border: 1px solid #48443F;
    border-radius: 9px;
}

QLabel#noticeIcon {
    color: #F2B39F;
    font-size: 15px;
}

QLabel#noticeLabel {
    color: #F2C2B4;
    font-weight: 600;
}

QToolButton#dismissNoticeButton {
    min-height: 22px;
    min-width: 22px;
    max-width: 22px;
    padding: 0;
    color: #B88E83;
}

QFrame#statusBar {
    background: #242522;
    border: 0;
    border-top: 1px solid #3A3B37;
}

QLabel#statusLabel {
    color: #9D9E98;
}

QLabel#workspaceStatusLabel[phase="ready"] {
    color: #85C891;
    font-weight: 600;
}

QLabel#workspaceStatusLabel[phase="busy"] {
    color: #E5B660;
    font-weight: 600;
}

QLabel#workspaceStatusLabel[phase="inactive"] {
    color: #858680;
}

QFrame#coloringPanel, QFrame#cameraPanel {
    background: #292A27;
    color: #F2F0EB;
    border: 1px solid #4A4B46;
    border-radius: 10px;
}

QFrame#coloringPanel QLabel, QFrame#cameraPanel QLabel {
    color: #E5E2DC;
}

QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QListWidget {
    min-height: 28px;
    color: #F2F0EB;
    background: #20211F;
    border: 1px solid #4A4B46;
    border-radius: 6px;
    padding: 0 8px;
    selection-background-color: #6252D2;
}

QFrame#coloringPanel QComboBox {
    combobox-popup: 0;
}

QFrame#coloringPanel QComboBox QAbstractItemView {
    color: #F2F0EB;
    background: #20211F;
    border: 1px solid #4A4B46;
    border-radius: 6px;
    outline: 0;
    padding: 4px 0;
    selection-color: #FFFFFF;
    selection-background-color: #6252D2;
}

QFrame#coloringPanel QComboBox QAbstractItemView::item {
    min-height: 30px;
    padding: 4px 10px;
    border: 0;
}

QFrame#coloringPanel QComboBox QAbstractItemView::item:hover {
    color: #FFFFFF;
    background: #383936;
}

QFrame#coloringPanel QComboBox QAbstractItemView::item:selected {
    color: #FFFFFF;
    background: #6252D2;
}

QTabBar::tab {
    color: #AAA9A3;
    background: #20211F;
    border: 1px solid #4A4B46;
    padding: 7px 10px;
}

QTabBar::tab:selected {
    color: #FFFFFF;
    background: #6252D2;
}

QMenu {
    color: #F2F0EB;
    background: #292A27;
    border: 1px solid #4A4B46;
    padding: 5px;
}

QMenu::item {
    padding: 7px 24px 7px 10px;
    border-radius: 5px;
}

QMenu::item:selected {
    background: #6252D2;
}
)QSS");
}
