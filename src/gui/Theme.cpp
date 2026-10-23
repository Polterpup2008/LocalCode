#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStringList>

namespace localcode::gui {

Theme& Theme::instance() {
    static Theme theme;
    return theme;
}

Theme::Theme() {
    buildPalette();
}

void Theme::setMode(ThemeMode mode) {
    mode_ = mode;
    buildPalette();
}

void Theme::setMonoPointSize(int pointSize) {
    monoPointSize_ = qBound(7, pointSize, 28);
}

void Theme::buildPalette() {
    Palette p;
    if (mode_ == ThemeMode::Dark) {
        // Neutral, very slightly warm greys rather than the blue-tinted set this
        // started with. A coding tool is looked at for hours; a colour cast in
        // the background fights every piece of syntax highlighting on top of it.
        // The surfaces sit close together on purpose - depth comes from spacing
        // and a hairline, not from stacking obviously different greys.
        p.window = QColor(0x13, 0x13, 0x12);
        p.surface = QColor(0x18, 0x18, 0x17);
        p.surfaceAlt = QColor(0x1E, 0x1E, 0x1D);
        p.surfaceRaised = QColor(0x26, 0x26, 0x25);
        p.border = QColor(0x27, 0x27, 0x26);
        p.borderStrong = QColor(0x38, 0x38, 0x36);

        p.text = QColor(0xE6, 0xE4, 0xE0);
        p.textMuted = QColor(0x9B, 0x98, 0x92);
        p.textFaint = QColor(0x8E, 0x8B, 0x85);
        p.textInverse = QColor(0x12, 0x12, 0x11);

        // One accent, warm, used sparingly: focus, selection, the send button.
        p.accent = QColor(0xC9, 0x64, 0x42);
        p.accentHover = QColor(0xD9, 0x77, 0x57);
        p.accentMuted = QColor(0x54, 0x31, 0x24);

        p.success = QColor(0x7F, 0xB0, 0x7F);
        p.warning = QColor(0xC9, 0xA2, 0x5E);
        p.danger = QColor(0xCE, 0x74, 0x6C);
        p.info = QColor(0x7F, 0x9C, 0xBF);

        p.addedBackground = QColor(0x1A, 0x2A, 0x1F);
        p.addedText = QColor(0x93, 0xC5, 0x9E);
        p.removedBackground = QColor(0x30, 0x1E, 0x1E);
        p.removedText = QColor(0xD5, 0x92, 0x8D);

        p.synKeyword = QColor(0xB6, 0xA0, 0xD8);
        p.synType = QColor(0x7F, 0xB8, 0xB0);
        p.synString = QColor(0xC4, 0xA6, 0x76);
        p.synNumber = QColor(0xD0, 0x9A, 0x7E);
        p.synComment = QColor(0x6B, 0x69, 0x63);
        p.synPreprocessor = QColor(0xC2, 0x8B, 0xA8);
        p.synFunction = QColor(0xD9, 0xC2, 0x8C);
    } else {
        // The same layout in a warm paper palette, with the same single accent,
        // so the two themes are recognisably one design.
        p.window = QColor(0xF7, 0xF6, 0xF3);
        p.surface = QColor(0xFD, 0xFC, 0xFA);
        p.surfaceAlt = QColor(0xF2, 0xF1, 0xED);
        p.surfaceRaised = QColor(0xE9, 0xE7, 0xE2);
        p.border = QColor(0xE0, 0xDE, 0xD8);
        p.borderStrong = QColor(0xC7, 0xC4, 0xBC);

        p.text = QColor(0x23, 0x22, 0x20);
        p.textMuted = QColor(0x63, 0x61, 0x5C);
        p.textFaint = QColor(0x74, 0x70, 0x68);
        p.textInverse = QColor(0xFD, 0xFC, 0xFA);

        p.accent = QColor(0xB0, 0x50, 0x30);
        p.accentHover = QColor(0xC9, 0x64, 0x42);
        p.accentMuted = QColor(0xF0, 0xDE, 0xD6);

        p.success = QColor(0x3D, 0x6E, 0x42);
        p.warning = QColor(0x8A, 0x62, 0x1C);
        p.danger = QColor(0xB3, 0x3F, 0x38);
        p.info = QColor(0x3B, 0x5F, 0x8A);

        p.addedBackground = QColor(0xE4, 0xF6, 0xE8);
        p.addedText = QColor(0x1C, 0x6B, 0x2E);
        p.removedBackground = QColor(0xFB, 0xE9, 0xEA);
        p.removedText = QColor(0xA3, 0x2E, 0x33);

        p.synKeyword = QColor(0x0B, 0x54, 0xC1);
        p.synType = QColor(0x0A, 0x6E, 0x6E);
        p.synString = QColor(0x8A, 0x5A, 0x00);
        p.synNumber = QColor(0x6A, 0x3D, 0xC0);
        p.synComment = QColor(0x66, 0x7A, 0x66);
        p.synPreprocessor = QColor(0x8A, 0x3D, 0x8A);
        p.synFunction = QColor(0x7A, 0x5A, 0x00);
    }
    palette_ = p;
}

QString Theme::firstAvailableFamily(const QStringList& candidates, const QString& fallback) {
    const auto families = QFontDatabase::families();
    for (const auto& candidate : candidates) {
        if (families.contains(candidate, Qt::CaseInsensitive)) return candidate;
    }
    return fallback;
}

QFont Theme::uiFont() const {
    static const QString family = firstAvailableFamily(
        {"Segoe UI Variable Text", "Segoe UI", "Inter", "Noto Sans", "DejaVu Sans"},
        QApplication::font().family());
    QFont font(family);
    font.setPointSize(10);
    return font;
}

QFont Theme::monoFont(int pointSize) const {
    static const QString family =
        firstAvailableFamily({"Cascadia Mono", "JetBrains Mono", "Consolas", "SF Mono",
                              "DejaVu Sans Mono", "Liberation Mono", "Courier New"},
                             QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    QFont font(family);
    font.setPointSize(pointSize > 0 ? pointSize : monoPointSize_);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QString Theme::richTextStyle() const {
    const auto font = uiFont();
    auto family = font.family();
    family.replace('\\', "\\\\");
    family.replace('\'', "\\'");
    return QString("font-family:'%1';font-size:%2pt;")
        .arg(family.toHtmlEscaped())
        .arg(font.pointSize());
}

void Theme::apply() {
    const auto& p = palette_;

    QPalette qt;
    qt.setColor(QPalette::Window, p.window);
    qt.setColor(QPalette::WindowText, p.text);
    qt.setColor(QPalette::Base, p.surfaceAlt);
    qt.setColor(QPalette::AlternateBase, p.surface);
    qt.setColor(QPalette::Text, p.text);
    qt.setColor(QPalette::PlaceholderText, p.textFaint);
    qt.setColor(QPalette::Button, p.surfaceAlt);
    qt.setColor(QPalette::ButtonText, p.text);
    qt.setColor(QPalette::Highlight, p.accent);
    qt.setColor(QPalette::HighlightedText, p.textInverse);
    qt.setColor(QPalette::ToolTipBase, p.surfaceRaised);
    qt.setColor(QPalette::ToolTipText, p.text);
    qt.setColor(QPalette::Link, p.accent);
    qt.setColor(QPalette::Disabled, QPalette::Text, p.textFaint);
    qt.setColor(QPalette::Disabled, QPalette::ButtonText, p.textFaint);
    qt.setColor(QPalette::Disabled, QPalette::WindowText, p.textFaint);

    QApplication::setPalette(qt);
    QApplication::setFont(uiFont());
    if (auto* app = qobject_cast<QApplication*>(QApplication::instance())) {
        app->setStyleSheet(styleSheet());
    }
}

QString Theme::styleSheet() const {
    const auto& p = palette_;
    auto c = [](const QColor& colour) { return colour.name(QColor::HexRgb); };

    return QString(R"(
QWidget {
    background: %1;
    color: %2;
}

QMainWindow::separator {
    background: %3;
    width: 5px;
    height: 5px;
}

QSplitter::handle {
    background: %3;
}
QSplitter::handle:horizontal { width: 5px; }
QSplitter::handle:vertical   { height: 5px; }
QSplitter::handle:hover { background: %4; }

/* --- Panels ------------------------------------------------------------ */
/* Flat: a panel is a background and some space, not a drawn box. Boxes inside
   boxes are what make a tool look like a form rather than a workspace. */
QFrame#Panel, QWidget#Panel {
    background: %5;
    border: none;
    border-radius: 0px;
}

QLabel#PanelTitle {
    color: %7;
    font-weight: 500;
    padding: 8px 10px 6px 10px;
    text-transform: uppercase;
    font-size: 9pt;
    letter-spacing: 0.09em;
}

QLabel#Muted   { color: %6; }
QLabel#Faint   { color: %7; }

/* --- Text surfaces ----------------------------------------------------- */
QPlainTextEdit, QTextEdit, QTextBrowser, QLineEdit, QAbstractItemView {
    background: %8;
    color: %2;
    border: 1px solid %3;
    border-radius: 4px;
    selection-background-color: %9;
    selection-color: %10;
}
QPlainTextEdit:focus, QTextEdit:focus, QLineEdit:focus {
    border: 1px solid %9;
}

/* --- Buttons ----------------------------------------------------------- */
QPushButton {
    background: %8;
    color: %2;
    border: 1px solid %4;
    border-radius: 4px;
    padding: 5px 12px;
    min-height: 20px;
}
QPushButton:hover   { background: %11; border-color: %9; }
QPushButton:pressed { background: %4; }
QPushButton:disabled { color: %7; border-color: %3; background: %5; }

QPushButton#Primary {
    background: %9;
    color: %10;
    border: 1px solid %9;
    font-weight: 500;
}
QPushButton#Primary:hover   { background: %12; border-color: %12; }
QPushButton#Primary:disabled { background: %13; border-color: %13; color: %7; }

/* Stop is a ghost button. It is pressed when something is already going wrong,
   and a solid red block sitting in the corner during every normal run reads as
   an alarm that never stops ringing. */
QPushButton#Danger {
    background: transparent;
    color: %6;
    border: 1px solid %3;
}
QPushButton#Danger:hover  { color: %14; border-color: %14; background: transparent; }
QPushButton#Danger:disabled { color: %7; border-color: %3; background: transparent; }

QPushButton#Flat {
    background: transparent;
    border: none;
    color: %6;
    padding: 3px 6px;
}
QPushButton#Flat:hover { color: %2; background: %11; }

QPushButton#JumpToLatest {
    background: %11;
    color: %2;
    border: 1px solid %4;
    border-radius: 12px;
    padding: 5px 14px;
}
QPushButton#JumpToLatest:hover { border-color: %9; }

/* --- Inputs ------------------------------------------------------------ */
QComboBox {
    background: %8;
    border: 1px solid %4;
    border-radius: 4px;
    padding: 4px 8px;
    min-height: 18px;
}
QComboBox:hover { border-color: %9; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: %11;
    border: 1px solid %4;
    selection-background-color: %9;
    outline: none;
}

QSpinBox, QDoubleSpinBox {
    background: %8;
    border: 1px solid %4;
    border-radius: 4px;
    padding: 3px 6px;
}

QCheckBox { spacing: 7px; }
QCheckBox::indicator {
    width: 14px; height: 14px;
    border: 1px solid %4;
    border-radius: 3px;
    background: %8;
}
QCheckBox::indicator:checked { background: %9; border-color: %9; }
QCheckBox::indicator:hover   { border-color: %9; }

/* --- Item views -------------------------------------------------------- */
QTreeView, QListView, QTableView {
    background: %5;
    border: none;
    outline: none;
    alternate-background-color: %5;
}
QTreeView::item, QListView::item {
    padding: 3px 4px;
    border: none;
}
QTreeView::item:hover, QListView::item:hover { background: %11; }
QTreeView::item:selected, QListView::item:selected {
    background: %13;
    color: %2;
}
QHeaderView::section {
    background: %5;
    color: %6;
    border: none;
    border-bottom: 1px solid %3;
    padding: 4px 6px;
    font-weight: 600;
}

/* --- Tabs -------------------------------------------------------------- */
/* A hairline under the bar instead of a box around the content: the tab strip
   should read as a row of labels, not as a folder tab from a filing cabinet. */
QTabWidget::pane {
    border: none;
    border-top: 1px solid %3;
    top: -1px;
}
QTabBar::tab {
    background: transparent;
    color: %7;
    padding: 8px 12px;
    border: none;
    border-bottom: 1px solid transparent;
}
QTabBar::tab:hover     { color: %2; }
QTabBar::tab:selected  {
    color: %2;
    border-bottom: 1px solid %9;
}
QTabBar::close-button { subcontrol-position: right; }

/* --- Scrollbars -------------------------------------------------------- */
QScrollBar:vertical {
    background: transparent;
    width: 11px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: %4;
    border-radius: 5px;
    min-height: 28px;
    margin: 2px;
}
QScrollBar::handle:vertical:hover { background: %6; }
QScrollBar:horizontal {
    background: transparent;
    height: 11px;
}
QScrollBar::handle:horizontal {
    background: %4;
    border-radius: 5px;
    min-width: 28px;
    margin: 2px;
}
QScrollBar::handle:horizontal:hover { background: %6; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* --- Menus, toolbars, status ------------------------------------------- */
QToolBar {
    background: %5;
    border: none;
    border-bottom: 1px solid %3;
    spacing: 6px;
    padding: 5px 8px;
}
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 4px 8px;
    color: %2;
}
QToolButton:hover   { background: %11; border-color: %4; }
QToolButton:pressed { background: %4; }
QToolButton#OpenProjectMenu { padding-right: 28px; }
QToolButton#OpenProjectMenu::menu-button {
    width: 22px;
    border: none;
    border-left: 1px solid %4;
    background: transparent;
}
QToolButton#OpenProjectMenu::menu-button:hover { background: %11; }

QMenuBar { background: %5; border-bottom: 1px solid %3; }
QMenuBar::item { padding: 5px 10px; background: transparent; }
QMenuBar::item:selected { background: %11; }

QMenu {
    background: %11;
    border: 1px solid %4;
    padding: 4px;
}
QMenu::item { padding: 5px 22px 5px 12px; border-radius: 3px; }
QMenu::item:selected { background: %9; color: %10; }
QMenu::separator { height: 1px; background: %3; margin: 4px 6px; }

QStatusBar {
    background: %5;
    border-top: 1px solid %3;
    color: %6;
}
QStatusBar::item { border: none; }

QToolTip {
    background: %11;
    color: %2;
    border: 1px solid %4;
    padding: 4px 6px;
}

/* --- Progress ---------------------------------------------------------- */
QProgressBar {
    background: %8;
    border: 1px solid %3;
    border-radius: 3px;
    height: 6px;
    text-align: center;
}
QProgressBar::chunk { background: %9; border-radius: 2px; }

QGroupBox {
    border: 1px solid %3;
    border-radius: 4px;
    margin-top: 14px;
    padding-top: 8px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 4px;
    color: %6;
    font-weight: 600;
}
)")
        .arg(c(p.window))          // 1
        .arg(c(p.text))            // 2
        .arg(c(p.border))          // 3
        .arg(c(p.borderStrong))    // 4
        .arg(c(p.surface))         // 5
        .arg(c(p.textMuted))       // 6
        .arg(c(p.textFaint))       // 7
        .arg(c(p.surfaceAlt))      // 8
        .arg(c(p.accent))          // 9
        .arg(c(p.textInverse))     // 10
        .arg(c(p.surfaceRaised))   // 11
        .arg(c(p.accentHover))     // 12
        .arg(c(p.accentMuted))     // 13
        .arg(c(p.danger));         // 14
}

}   // namespace localcode::gui
