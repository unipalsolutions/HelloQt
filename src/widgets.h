#pragma once

#include "icons.h"

#include <QAbstractButton>
#include <QLayout>
#include <QPointer>
#include <QStackedWidget>
#include <QWidget>

class QVariantAnimation;

// زر مرسوم يدوياً بانتقالات ناعمة عند المرور والضغط والتحديد
class FancyButton : public QAbstractButton
{
    Q_OBJECT

public:
    enum class Kind { Primary, Secondary, Danger, Nav };

    FancyButton(Icons::Name icon, const QString &text, Kind kind, QWidget *parent = nullptr);

    QSize sizeHint() const override;
    void setIconName(Icons::Name icon);
    // في الشاشات الضيقة تظهر الأيقونة وحدها، والنص في التلميح
    void setCompact(bool compact);
    bool isCompact() const { return m_compact; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QVariantAnimation *makeAnimation(double *value, int duration);
    static void animate(QVariantAnimation *animation, double from, double to);

    Icons::Name m_icon;
    Kind m_kind;
    bool m_compact = false;
    double m_hover = 0.0;
    double m_press = 0.0;
    double m_check = 0.0;
    QVariantAnimation *m_hoverAnimation;
    QVariantAnimation *m_pressAnimation;
    QVariantAnimation *m_checkAnimation;
};

// يرتب العناصر في سطر واحد، وينقل ما لا يتسع منها إلى سطر جديد بدل أن يختفي في الشاشات الصغيرة
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = 12, int vSpacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;

    QList<QLayoutItem *> m_items;
    int m_hSpacing;
    int m_vSpacing;
};

// مفتاح تشغيل/إيقاف بحركة انزلاق
class ToggleSwitch : public QAbstractButton
{
    Q_OBJECT

public:
    explicit ToggleSwitch(const QString &text, QWidget *parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_position = 0.0;
    QVariantAnimation *m_animation;
};

// تبديل الصفحات بانزلاق وتلاشٍ
class AnimatedStack : public QStackedWidget
{
    Q_OBJECT

public:
    using QStackedWidget::QStackedWidget;

    void slideTo(int index);
};

// إشعار صغير ينزلق من أسفل النافذة ثم يختفي
class Toast : public QWidget
{
    Q_OBJECT

public:
    enum class Kind { Success, Warning, Info };

    static void show(QWidget *host, const QString &text, Kind kind = Kind::Success);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Toast(QWidget *host, const QString &text, Kind kind);

    QString m_text;
    Kind m_kind;
    static QPointer<Toast> s_current;
};
