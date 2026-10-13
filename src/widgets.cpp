#include "widgets.h"

#include "theme.h"

#include <QEasingCurve>
#include <QEnterEvent>
#include <QGraphicsOpacityEffect>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QVariantAnimation>

#include <algorithm>

// ───────────── FancyButton ─────────────

FancyButton::FancyButton(Icons::Name icon, const QString &text, Kind kind, QWidget *parent)
    : QAbstractButton(parent)
    , m_icon(icon)
    , m_kind(kind)
    , m_hoverAnimation(makeAnimation(&m_hover, 180))
    , m_pressAnimation(makeAnimation(&m_press, 110))
    , m_checkAnimation(makeAnimation(&m_check, 260))
{
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setCheckable(kind == Kind::Nav);
    setSizePolicy(kind == Kind::Nav ? QSizePolicy::Expanding : QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(this, &QAbstractButton::pressed, this, [this] { animate(m_pressAnimation, m_press, 1.0); });
    connect(this, &QAbstractButton::released, this, [this] { animate(m_pressAnimation, m_press, 0.0); });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) { animate(m_checkAnimation, m_check, on ? 1.0 : 0.0); });
}

void FancyButton::setIconName(Icons::Name icon)
{
    m_icon = icon;
    update();
}

QVariantAnimation *FancyButton::makeAnimation(double *value, int duration)
{
    auto *animation = new QVariantAnimation(this);
    animation->setDuration(duration);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this, [this, value](const QVariant &v) {
        *value = v.toDouble();
        update();
    });
    return animation;
}

void FancyButton::animate(QVariantAnimation *animation, double from, double to)
{
    animation->stop();
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->start();
}

QSize FancyButton::sizeHint() const
{
    const int textWidth = fontMetrics().horizontalAdvance(text());
    if (m_kind == Kind::Nav)
        return QSize(textWidth + 70, 46);
    return QSize(textWidth + (text().isEmpty() ? 22 : 56), 40);
}

void FancyButton::enterEvent(QEnterEvent *event)
{
    animate(m_hoverAnimation, m_hover, 1.0);
    QAbstractButton::enterEvent(event);
}

void FancyButton::leaveEvent(QEvent *event)
{
    animate(m_hoverAnimation, m_hover, 0.0);
    QAbstractButton::leaveEvent(event);
}

void FancyButton::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::EnabledChange) {
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (!isEnabled())
            m_hover = 0.0;
        update();
    }
    QAbstractButton::changeEvent(event);
}

void FancyButton::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
    const QPointF center = bounds.center();
    const double scale = 1.0 - 0.04 * m_press;
    p.translate(center);
    p.scale(scale, scale);
    p.translate(-center);

    const bool enabled = isEnabled();
    const bool rtl = isRightToLeft();
    QColor foreground = Text;
    QColor iconColor = Text;
    QPainterPath shape;
    shape.addRoundedRect(bounds, 11, 11);

    switch (m_kind) {
    case Kind::Primary: {
        if (!enabled) {
            p.fillPath(shape, SurfaceHigh);
            foreground = iconColor = Muted;
            break;
        }
        QLinearGradient gradient(bounds.topLeft(), bounds.topRight());
        gradient.setColorAt(rtl ? 1.0 : 0.0, Accent);
        gradient.setColorAt(rtl ? 0.0 : 1.0, mix(Accent, Accent2, 0.55 + 0.45 * m_hover));
        p.fillPath(shape, gradient);
        p.fillPath(shape, withAlpha(Qt::white, int(28 * m_hover)));
        foreground = iconColor = Qt::white;
        break;
    }
    case Kind::Secondary:
        p.fillPath(shape, enabled ? mix(SurfaceHigh, QColor(0x1a, 0x38, 0x66), m_hover) : Surface);
        p.setPen(QPen(enabled ? mix(Border, Accent, m_hover) : Border, 1.0));
        p.drawPath(shape);
        foreground = enabled ? Text : Muted;
        iconColor = enabled ? mix(Accent2, Text, 0.3 - 0.3 * m_hover) : Muted;
        break;
    case Kind::Danger:
        p.fillPath(shape, withAlpha(Danger, enabled ? int(30 + 60 * m_hover) : 14));
        p.setPen(QPen(withAlpha(Danger, enabled ? int(90 + 120 * m_hover) : 40), 1.0));
        p.drawPath(shape);
        foreground = iconColor = enabled ? mix(QColor(0xbf, 0xdb, 0xfe), Qt::white, m_hover) : Muted;
        break;
    case Kind::Nav: {
        const double active = std::max(m_hover * 0.6, m_check);
        p.fillPath(shape, withAlpha(m_check > 0.01 ? Accent : SurfaceHigh, int(m_check > 0.01 ? 50 * m_check : 255 * m_hover)));
        if (m_check > 0.01) {
            const double h = bounds.height() * 0.55 * m_check;
            const QRectF bar(rtl ? bounds.right() - 4 : bounds.left() + 1, center.y() - h / 2, 3, h);
            p.setPen(Qt::NoPen);
            p.setBrush(Accent2);
            p.drawRoundedRect(bar, 1.5, 1.5);
        }
        foreground = mix(Muted, Text, active);
        iconColor = mix(Muted, Accent2, m_check > 0 ? m_check : m_hover * 0.5);
        break;
    }
    }

    // الأيقونة بجانب النص، تبدأ من اليمين في الواجهة العربية
    QFont font = this->font();
    font.setWeight(m_kind == Kind::Nav ? QFont::DemiBold : QFont::Medium);
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const double iconSize = 18.0;
    const double spacing = text().isEmpty() ? 0.0 : 9.0;
    const double contentWidth = iconSize + spacing + metrics.horizontalAdvance(text());
    const double start = m_kind == Kind::Nav ? 18.0 : (bounds.width() - contentWidth) / 2.0;
    const double iconX = rtl ? bounds.right() - start - iconSize : bounds.left() + start;
    Icons::paint(p, m_icon, QRectF(iconX, center.y() - iconSize / 2, iconSize, iconSize), iconColor);

    const double textWidth = metrics.horizontalAdvance(text()) + 2;
    const double textX = rtl ? iconX - spacing - textWidth : iconX + iconSize + spacing;
    p.setPen(foreground);
    p.drawText(QRectF(textX, bounds.top(), textWidth, bounds.height()), Qt::AlignCenter, text());
}

// ───────────── ToggleSwitch ─────────────

ToggleSwitch::ToggleSwitch(const QString &text, QWidget *parent)
    : QAbstractButton(parent)
    , m_animation(new QVariantAnimation(this))
{
    setText(text);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    m_animation->setDuration(220);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_position = v.toDouble();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_animation->stop();
        m_animation->setStartValue(m_position);
        m_animation->setEndValue(on ? 1.0 : 0.0);
        m_animation->start();
    });
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(44 + 10 + fontMetrics().horizontalAdvance(text()) + 4, 30);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool rtl = isRightToLeft();

    const QRectF track(rtl ? width() - 44 : 0, (height() - 24) / 2.0, 44, 24);
    p.setPen(Qt::NoPen);
    p.setBrush(mix(Border, Accent, m_position));
    p.drawRoundedRect(track, 12, 12);

    // في الواجهة العربية وضع التشغيل على اليسار
    const double travel = track.width() - 24;
    const double offset = rtl ? travel * (1.0 - m_position) : travel * m_position;
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(track.left() + 3 + offset, track.top() + 3, 18, 18));

    p.setPen(isEnabled() ? Text : Muted);
    const QRectF textRect = rtl ? QRectF(0, 0, track.left() - 10, height()) : QRectF(track.right() + 10, 0, width(), height());
    p.drawText(textRect, (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter | Qt::AlignAbsolute, text());
}

// ───────────── AnimatedStack ─────────────

void AnimatedStack::slideTo(int index)
{
    if (index == currentIndex() || index < 0 || index >= count())
        return;

    const bool forward = index > currentIndex();
    QWidget *next = widget(index);
    setCurrentIndex(index);

    auto *effect = new QGraphicsOpacityEffect(next);
    next->setGraphicsEffect(effect);

    const QPoint end = next->pos();
    const int shift = (forward == isRightToLeft() ? 1 : -1) * 48;

    auto *fade = new QPropertyAnimation(effect, "opacity");
    fade->setDuration(320);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);

    auto *slide = new QPropertyAnimation(next, "pos");
    slide->setDuration(380);
    slide->setStartValue(end + QPoint(shift, 0));
    slide->setEndValue(end);
    slide->setEasingCurve(QEasingCurve::OutCubic);

    auto *group = new QParallelAnimationGroup(next);
    group->addAnimation(fade);
    group->addAnimation(slide);
    connect(group, &QAbstractAnimation::finished, next, [next] { next->setGraphicsEffect(nullptr); });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

// ───────────── Toast ─────────────

QPointer<Toast> Toast::s_current;

Toast::Toast(QWidget *host, const QString &text, Kind kind)
    : QWidget(host)
    , m_text(text)
    , m_kind(kind)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont f = font();
    f.setPointSizeF(10.5);
    f.setWeight(QFont::Medium);
    setFont(f);
    const int width = std::min(host->width() - 40, fontMetrics().horizontalAdvance(text) + 80);
    resize(width, 52);
}

void Toast::show(QWidget *host, const QString &text, Kind kind)
{
    if (s_current)
        s_current->deleteLater();
    auto *toast = new Toast(host, text, kind);
    s_current = toast;

    const QPoint end((host->width() - toast->width()) / 2, host->height() - toast->height() - 44);
    toast->move(end + QPoint(0, 24));
    auto *effect = new QGraphicsOpacityEffect(toast);
    effect->setOpacity(0.0);
    toast->setGraphicsEffect(effect);
    toast->QWidget::show();
    toast->raise();

    auto *fadeIn = new QPropertyAnimation(effect, "opacity");
    fadeIn->setDuration(260);
    fadeIn->setEndValue(1.0);
    auto *slideIn = new QPropertyAnimation(toast, "pos");
    slideIn->setDuration(380);
    slideIn->setEndValue(end);
    slideIn->setEasingCurve(QEasingCurve::OutBack);
    auto *enter = new QParallelAnimationGroup;
    enter->addAnimation(fadeIn);
    enter->addAnimation(slideIn);

    auto *fadeOut = new QPropertyAnimation(effect, "opacity");
    fadeOut->setDuration(300);
    fadeOut->setEndValue(0.0);

    auto *sequence = new QSequentialAnimationGroup(toast);
    sequence->addAnimation(enter);
    sequence->addPause(2800);
    sequence->addAnimation(fadeOut);
    connect(sequence, &QAbstractAnimation::finished, toast, &QObject::deleteLater);
    sequence->start();
}

void Toast::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor color = m_kind == Kind::Success ? Success : m_kind == Kind::Warning ? Warning : Accent2;
    const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setPen(QPen(withAlpha(color, 120), 1.0));
    p.setBrush(QColor(0x15, 0x28, 0x4a));
    p.drawRoundedRect(bounds, 14, 14);

    const bool rtl = isRightToLeft();
    const QRectF badge(rtl ? bounds.right() - 40 : bounds.left() + 12, bounds.center().y() - 14, 28, 28);
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(color, 50));
    p.drawEllipse(badge);
    const Icons::Name icon = m_kind == Kind::Success ? Icons::Name::Check : m_kind == Kind::Warning ? Icons::Name::Alert : Icons::Name::Info;
    Icons::paint(p, icon, badge.adjusted(6, 6, -6, -6), color);

    p.setPen(Text);
    const QRectF textRect = rtl ? QRectF(bounds.left() + 16, 0, badge.left() - bounds.left() - 26, height())
                                : QRectF(badge.right() + 10, 0, bounds.right() - badge.right() - 26, height());
    p.drawText(textRect, (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter | Qt::AlignAbsolute, m_text);
}
