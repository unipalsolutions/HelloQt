#pragma once

#include "icons.h"

#include <QAbstractButton>
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
    double m_hover = 0.0;
    double m_press = 0.0;
    double m_check = 0.0;
    QVariantAnimation *m_hoverAnimation;
    QVariantAnimation *m_pressAnimation;
    QVariantAnimation *m_checkAnimation;
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
