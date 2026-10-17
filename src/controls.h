#pragma once

#include "icons.h"

#include <QComboBox>
#include <QSlider>
#include <QWidget>

#include <functional>

class QLabel;
class QScrollArea;
class QVBoxLayout;
class QVariantAnimation;

// عناصر إدخال مرسومة يدوياً بحركات ناعمة، بدل عناصر Qt التقليدية

// أزرار متلاصقة يُختار واحد منها، وتنزلق الكبسولة المضيئة إلى المختار
class SegmentedControl : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentedControl(const QStringList &items, QWidget *parent = nullptr);

    int currentIndex() const { return m_current; }
    void setCurrentIndex(int index);
    int count() const { return int(m_items.size()); }
    void setItemText(int index, const QString &text);
    void setItemToolTip(int index, const QString &tip);
    // يملأ العرض المتاح بأجزاء متساوية بدل أن يأخذ عرض نصوصه فقط
    void setExpanding(bool expanding);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    QRectF segmentRect(int index) const;
    int indexAt(const QPoint &pos) const;
    int itemWidth(int index) const;

    QStringList m_items;
    QStringList m_tips;
    int m_current = 0;
    int m_hovered = -1;
    bool m_expanding = false;
    double m_position = 0.0; // موضع الكبسولة بوحدة الأجزاء، يتحرك بين القديم والجديد
    QVariantAnimation *m_animation;
};

// عدّاد بزرين «−» و«+» حول القيمة، ويقبل العجلة والأسهم
class Stepper : public QWidget
{
    Q_OBJECT

public:
    explicit Stepper(QWidget *parent = nullptr);

    int value() const { return m_value; }
    void setValue(int value);
    void setRange(int minimum, int maximum);
    // نص يظهر بدل الحد الأدنى، مثل «تلقائي» بدل 0
    void setSpecialValueText(const QString &text);
    void setSuffix(const QString &suffix);

    QSize sizeHint() const override;

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QString text() const;
    QRectF minusRect() const;
    QRectF plusRect() const;

    int m_value = 0;
    int m_minimum = 0;
    int m_maximum = 99;
    QString m_special;
    QString m_suffix;
    int m_hovered = 0; // -1 ناقص، 1 زائد
};

// شريط تمرير مرسوم بفقاعة تعرض القيمة فوق المقبض
class ValueSlider : public QSlider
{
    Q_OBJECT

public:
    explicit ValueSlider(QWidget *parent = nullptr);

    // نص الفقاعة؛ الافتراضي القيمة مع «%»
    void setFormatter(std::function<QString(int)> formatter);
    // لون المقطع الممتلئ والمقبض (يتغير للتحذير مثلاً)
    void setAccent(const QColor &color);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF grooveRect() const;
    int valueAt(double x) const;

    std::function<QString(int)> m_formatter;
    QColor m_accent;
    double m_hover = 0.0;
    QVariantAnimation *m_hoverAnimation;
};

// قائمة منسدلة مرسومة: إطار مستدير وسهم يدور عند الفتح
class SelectBox : public QComboBox
{
    Q_OBJECT

public:
    explicit SelectBox(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void showPopup() override;
    void hidePopup() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    bool m_hovered = false;
    bool m_open = false;
};

// قسم بعنوان يُطوى ويُفتح بحركة، ويتذكر حالته إن أُعطي مفتاحاً
class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    CollapsibleSection(const QString &title, QWidget *parent = nullptr, const QString &settingsKey = QString());

    QVBoxLayout *contentLayout() const { return m_contentLayout; }
    QWidget *content() const { return m_content; }
    // عناصر في سطر العنوان (مثل أزرار الخط الزمني)، لا تطوي القسم عند النقر عليها
    void addHeaderWidget(QWidget *widget, int stretch = 0);
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded, bool animated = true);
    void setTitle(const QString &title);
    // بلا إطار: للأقسام داخل بطاقة قائمة بالفعل
    void setFlat(bool flat);

signals:
    void expandedChanged(bool expanded);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyHeight(double progress);

    QWidget *m_header;
    QLabel *m_title;
    QLabel *m_chevron;
    QWidget *m_content;
    QVBoxLayout *m_contentLayout;
    QString m_settingsKey;
    bool m_expanded = true;
    bool m_flat = false;
    double m_progress = 1.0;
    QVariantAnimation *m_animation;
};

// لوحة تنزلق من طرف النافذة فوق المحتوى مع تعتيم خلفها، وتُغلق بالنقر خارجها أو Esc
class SideDrawer : public QWidget
{
    Q_OBJECT

public:
    SideDrawer(Icons::Name icon, const QString &title, QWidget *host);

    QVBoxLayout *bodyLayout() const { return m_bodyLayout; }
    QVBoxLayout *footerLayout() const { return m_footerLayout; }
    bool isOpen() const { return m_open; }
    void setOpen(bool open);
    void toggle() { setOpen(!m_open); }
    void setPanelWidth(int width) { m_panelWidth = width; }

signals:
    void openChanged(bool open);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void layoutPanel();

    QWidget *m_panel;
    QVBoxLayout *m_bodyLayout;
    QVBoxLayout *m_footerLayout;
    int m_panelWidth = 360;
    bool m_open = false;
    double m_progress = 0.0;
    QVariantAnimation *m_animation;
};

// حلقة تقدم صغيرة لحالة التحليل؛ قيمة سالبة تعني دوراناً بلا نسبة معروفة
class ProgressRing : public QWidget
{
    Q_OBJECT

public:
    explicit ProgressRing(QWidget *parent = nullptr);

    void setProgress(double progress);
    // بدل الحلقة: أيقونة الحالة حين لا يجري عمل
    void setIcon(Icons::Name icon, const QColor &color);

    QSize sizeHint() const override { return {26, 26}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_progress = -2.0; // -2: أيقونة، -1: دوران
    double m_spin = 0.0;
    Icons::Name m_icon = Icons::Name::Info;
    QColor m_color;
    QVariantAnimation *m_spinAnimation;
};
