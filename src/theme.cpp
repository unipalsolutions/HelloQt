#include "theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

#include <algorithm>

namespace Theme {

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
                            float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t),
                            float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(std::clamp(alpha, 0, 255));
    return color;
}

static QString styleSheet()
{
    QString css = QStringLiteral(R"(
QMainWindow, QDialog, QMessageBox, QProgressDialog, QInputDialog { background: @bg; }
QWidget { color: @text; }
QFrame#sidebar { background: @sidebar; border: none; border-left: 1px solid @border; }
QFrame#card { background: @surface; border: 1px solid @border; border-radius: 16px; }
QFrame#viewCard { background: @viewport; border: 1px solid @border; border-radius: 16px; }
QFrame#statCard { background: @surfaceHigh; border: 1px solid @border; border-radius: 14px; }
QFrame#tabStrip { background: @surface; border: 1px solid @border; border-radius: 21px; }
QFrame#separator { background: @border; max-width: 1px; min-width: 1px; }
QLabel { background: transparent; }
QLabel#title { font-size: 20pt; font-weight: 700; }
QLabel#subtitle, QLabel#muted { color: @muted; }
QLabel#sectionTitle { font-size: 12pt; font-weight: 600; }
QLabel#statValue { font-size: 18pt; font-weight: 700; color: @text; }
QLabel#badge { background: @accentSoft; color: @accent2; border-radius: 10px; padding: 2px 10px; font-weight: 600; }
QLineEdit, QSpinBox, QPlainTextEdit, QComboBox, QAbstractSpinBox {
    background: @surfaceHigh; border: 1px solid @border; border-radius: 12px;
    padding: 7px 12px; color: @text; selection-background-color: @accent;
}
QLineEdit:hover, QAbstractSpinBox:hover, QPlainTextEdit:hover, QComboBox:hover { border-color: @borderHover; }
QLineEdit:focus, QAbstractSpinBox:focus, QPlainTextEdit:focus, QComboBox:focus { border: 1px solid @accent2; }
QAbstractSpinBox#timeRange { padding: 5px 10px; min-width: 74px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0; border: none; }
QLineEdit#timeField {
    background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 4px 8px;
    font-weight: 600; font-family: "DejaVu Sans Mono", "Consolas", monospace;
}
QLineEdit#timeField:hover { background: @surfaceHigh; border-color: @border; }
QLineEdit#timeField:focus { background: @surfaceHigh; border-color: @accent2; }
QSpinBox { padding: 5px 8px; min-width: 52px; }
QSpinBox::up-button, QSpinBox::down-button { width: 16px; border: none; background: transparent; }
QComboBox QAbstractItemView {
    background: @surfaceHigh; border: 1px solid @border; selection-background-color: @accent; outline: 0;
}
QListWidget, QListView { background: transparent; border: none; outline: 0; }
QListWidget#photoGrid::item { border-radius: 12px; padding: 6px; color: @muted; }
QListWidget#photoGrid::item:hover { background: @surfaceHigh; }
QListWidget#photoGrid::item:selected { background: @accentSoft; color: @text; }
QListWidget#plainList::item { border-radius: 10px; padding: 8px 10px; margin: 1px 0; }
QListWidget#plainList::item:hover { background: @surfaceHigh; }
QListWidget#plainList::item:selected { background: @accentSoft; color: @text; }
QTabWidget::pane { border: 1px solid @border; border-radius: 14px; background: @surface; top: 8px; }
QTabWidget::tab-bar { alignment: right; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab {
    background: transparent; color: @muted; border: 1px solid transparent; border-radius: 15px;
    padding: 6px 16px; margin: 0 2px;
}
QTabBar::tab:hover { background: @surfaceHigh; color: @text; }
QTabBar::tab:selected { background: @surfaceHigh; border-color: @border; color: @text; font-weight: 600; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: @border; border-radius: 4px; min-height: 30px; min-width: 30px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @borderHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QSlider::groove:horizontal { height: 6px; background: @border; border-radius: 3px; }
QSlider::sub-page:horizontal { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent2, stop:1 @accent); border-radius: 3px; }
QSlider::add-page:horizontal { background: @border; border-radius: 3px; }
QSlider::handle:horizontal { background: white; width: 12px; height: 12px; margin: -6px 0; border-radius: 9px; border: 3px solid @accent; }
QProgressBar#thinProgress { background: transparent; border: none; border-radius: 2px; max-height: 4px; min-height: 4px; }
QProgressBar#thinProgress::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent2, stop:1 @accent); border-radius: 2px; }
QStatusBar { background: @sidebar; color: @muted; border-top: 1px solid @border; }
QStatusBar::item { border: none; }
QToolTip { background: @surfaceHigh; color: @text; border: 1px solid @border; border-radius: 8px; padding: 6px 10px; }
QSplitter::handle { background: transparent; }
QPushButton {
    background: @surfaceHigh; color: @text; border: 1px solid @border; border-radius: 18px;
    padding: 8px 18px; min-width: 70px;
}
QPushButton:hover { border-color: @borderHover; background: @surfaceHover; }
QPushButton:default { background: @accent; border-color: @accent; color: white; }
QPushButton:default:hover { background: @accentHover; }
QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px; border: 1px solid @borderHover; background: @surfaceHigh; }
QCheckBox::indicator:hover { border-color: @accent2; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/resources/check.png); }
QCheckBox::indicator:disabled { background: @surface; border-color: @border; }
QAbstractItemView::indicator { width: 16px; height: 16px; border-radius: 5px; border: 1px solid @borderHover; background: @surfaceHigh; }
QAbstractItemView::indicator:checked { background: @accent; border-color: @accent; image: url(:/resources/check.png); }
QRadioButton::indicator { width: 16px; height: 16px; border-radius: 9px; border: 1px solid @borderHover; background: @surfaceHigh; }
QRadioButton::indicator:checked { width: 8px; height: 8px; border: 5px solid @accent; background: white; }
/* السهم في الطرف الأيسر كقوائم SelectBox؛ القائمة المنسدلة وحقل التاريخ ينعكسان مع الاتجاه العربي */
QComboBox::drop-down, QDateTimeEdit::drop-down { subcontrol-origin: padding; width: 28px; border: none; background: transparent; }
QComboBox::drop-down, QDateTimeEdit::drop-down { subcontrol-position: center right; }
QDateTimeEdit[calendarPopup="true"] { min-width: 96px; }
/* حقول الوقت تُعدَّل بالكتابة والعجلة؛ أسهم Fusion الصغيرة تظهر كخدش في الزاوية */
QDateTimeEdit::up-arrow, QDateTimeEdit::down-arrow { image: none; width: 0; height: 0; }
QComboBox::down-arrow, QDateTimeEdit[calendarPopup="true"]::down-arrow { image: url(:/resources/chevron-down.png); width: 14px; height: 14px; }
QHeaderView { background: transparent; border: none; }
QHeaderView::section {
    background: @surfaceHigh; color: @muted; border: none; border-bottom: 1px solid @border;
    padding: 8px 10px; font-weight: 600;
}
QTableView, QTreeView, QTableWidget, QTreeWidget {
    background: @surface; alternate-background-color: @surfaceAlt; border: 1px solid @border; border-radius: 12px;
    gridline-color: @border; selection-background-color: @accentSoft; selection-color: @text; outline: 0;
}
QTableCornerButton::section { background: @surfaceHigh; border: none; }
QMenu { background: @surface; border: 1px solid @border; padding: 6px; }
QMenu::item { padding: 7px 26px 7px 18px; border-radius: 8px; }
QMenu::item:selected { background: @accentSoft; }
QMenu::item:disabled { color: @muted; }
QMenu::separator { height: 1px; background: @border; margin: 5px 8px; }
QToolButton#tab {
    background: transparent; color: @muted; border: 1px solid @border; border-radius: 13px; padding: 3px 10px;
}
QToolButton#tab:hover { color: @text; border-color: @borderHover; }
QToolButton#tab:checked { background: @accentSoft; color: @text; border-color: @accent2; font-weight: 600; }
QToolButton#chip {
    background: @surfaceHigh; color: @text; border: 1px solid @border; border-radius: 9px; padding: 2px 8px;
}
QToolButton#chip:hover { border-color: @accent2; color: @accent2; }
QToolButton#chip:checked { border-color: @highlight; background: @highlightSoft; }
)");
    const QList<QPair<QString, QColor>> colors{
        {QStringLiteral("@accentSoft"), withAlpha(Accent, 70)},
        {QStringLiteral("@accentHover"), mix(Accent, Accent2, 0.35)},
        {QStringLiteral("@highlightSoft"), withAlpha(Highlight, 45)},
        {QStringLiteral("@highlight"), Highlight},
        {QStringLiteral("@borderHover"), mix(Border, Muted, 0.45)},
        {QStringLiteral("@surfaceHover"), mix(SurfaceHigh, Border, 0.6)},
        {QStringLiteral("@viewport"), Viewport},
        {QStringLiteral("@surfaceAlt"), mix(Surface, SurfaceHigh, 0.45)},
        {QStringLiteral("@surfaceHigh"), SurfaceHigh}, {QStringLiteral("@surface"), Surface},
        {QStringLiteral("@sidebar"), Sidebar},         {QStringLiteral("@border"), Border},
        {QStringLiteral("@accent2"), Accent2},         {QStringLiteral("@accent"), Accent},
        {QStringLiteral("@muted"), Muted},             {QStringLiteral("@text"), Text},
        {QStringLiteral("@bg"), Background},
    };
    for (const auto &[name, color] : colors)
        css.replace(name, color.alpha() < 255 ? QStringLiteral("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha())
                                                  : color.name());
    return css;
}

// Fusion دون أيقونات Qt القديمة على أزرار «حفظ» و«إلغاء» في النوافذ الحوارية
class SoftStyle : public QProxyStyle
{
public:
    SoftStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *data) const override
    {
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};

void apply(QApplication &app)
{
    QApplication::setStyle(new SoftStyle);

    QPalette palette;
    palette.setColor(QPalette::Window, Background);
    palette.setColor(QPalette::WindowText, Text);
    palette.setColor(QPalette::Base, Surface);
    palette.setColor(QPalette::AlternateBase, SurfaceHigh);
    palette.setColor(QPalette::Text, Text);
    palette.setColor(QPalette::Button, SurfaceHigh);
    palette.setColor(QPalette::ButtonText, Text);
    palette.setColor(QPalette::Highlight, Accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, SurfaceHigh);
    palette.setColor(QPalette::ToolTipText, Text);
    palette.setColor(QPalette::PlaceholderText, Muted);
    palette.setColor(QPalette::Disabled, QPalette::Text, Muted);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, Muted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, Muted);
    QApplication::setPalette(palette);

    QFont font = QApplication::font();
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans Arabic"),
                      QStringLiteral("Noto Naskh Arabic"), QStringLiteral("Tahoma")});
    font.setPointSizeF(10);
    QApplication::setFont(font);

    app.setStyleSheet(styleSheet());
}

} // namespace Theme
