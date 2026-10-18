#include "controls.h"

#include "appsettings.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTextOption>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

QVariantAnimation *animation(QObject *owner, int duration, std::function<void(double)> apply)
{
    auto *a = new QVariantAnimation(owner);
    a->setDuration(duration);
    a->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(a, &QVariantAnimation::valueChanged, owner, [apply = std::move(apply)](const QVariant &v) { apply(v.toDouble()); });
    return a;
}

void run(QVariantAnimation *a, double from, double to)
{
    a->stop();
    a->setStartValue(from);
    a->setEndValue(to);
    a->start();
}

// سهم «⌄» مرسوم بزاوية دوران: 0 للأسفل
void paintChevron(QPainter &p, const QPointF &center, double size, double angle, const QColor &color)
{
    p.save();
    p.translate(center);
    p.rotate(angle);
    QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double h = size / 2.0;
    p.drawPolyline(QPolygonF{QPointF(-h, -h / 2.0), QPointF(0, h / 2.0), QPointF(h, -h / 2.0)});
    p.restore();
}

} // namespace

// ───────────── SegmentedControl ─────────────

SegmentedControl::SegmentedControl(const QStringList &items, QWidget *parent)
    : QWidget(parent)
    , m_items(items)
    , m_tips(items.size())
{
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_animation = animation(this, 240, [this](double v) {
        m_position = v;
        update();
    });
}

void SegmentedControl::setCurrentIndex(int index)
{
    index = std::clamp(index, 0, std::max(0, count() - 1));
    if (index == m_current)
        return;
    const int old = m_current;
    m_current = index;
    if (isVisible())
        run(m_animation, m_position, index);
    else
        m_position = index;
    update();
    Q_UNUSED(old)
    emit currentChanged(index);
}

void SegmentedControl::setItemText(int index, const QString &text)
{
    if (index < 0 || index >= count() || m_items.at(index) == text)
        return;
    m_items[index] = text;
    updateGeometry();
    update();
}

void SegmentedControl::setItemToolTip(int index, const QString &tip)
{
    if (index >= 0 && index < count())
        m_tips[index] = tip;
}

void SegmentedControl::setExpanding(bool expanding)
{
    m_expanding = expanding;
    setSizePolicy(expanding ? QSizePolicy::Expanding : QSizePolicy::Fixed, QSizePolicy::Fixed);
    updateGeometry();
}

int SegmentedControl::itemWidth(int index) const
{
    return fontMetrics().horizontalAdvance(m_items.at(index)) + 22;
}

QSize SegmentedControl::sizeHint() const
{
    int width = 6;
    for (int i = 0; i < count(); ++i)
        width += itemWidth(i);
    return {width, 32};
}

QSize SegmentedControl::minimumSizeHint() const
{
    if (!m_expanding)
        return sizeHint();
    int widest = 0;
    for (int i = 0; i < count(); ++i)
        widest = std::max(widest, itemWidth(i) - 8);
    return {widest * count() + 6, 32};
}

QRectF SegmentedControl::segmentRect(int index) const
{
    // الأجزاء من اليمين إلى اليسار في الواجهة العربية
    const QRectF inner = QRectF(rect()).adjusted(3, 3, -3, -3);
    // الممتد أجزاؤه متساوية، فإن ضاق عنها أخذ كل جزء عرض نصه ووُزّع الباقي بالتساوي،
    // فلا يُختصر نص قصير لأن جاره أطول منه
    const bool equal = m_expanding && inner.width() >= sizeHint().width() - 6;
    double total = 0.0;
    QList<double> widths;
    for (int i = 0; i < count(); ++i) {
        widths << (equal ? 1.0 : double(itemWidth(i)));
        total += widths.last();
    }
    if (m_expanding && !equal) {
        double text = 0.0;
        for (int i = 0; i < count(); ++i) {
            widths[i] = fontMetrics().horizontalAdvance(m_items.at(i)) * 1.06 + 4; // الجزء المحدد بخط أعرض
            text += widths.at(i);
        }
        const double pad = std::max(0.0, (inner.width() - text) / count());
        total = 0.0;
        for (double &w : widths) {
            w += pad;
            total += w;
        }
    }
    double start = 0.0;
    for (int i = 0; i < index; ++i)
        start += widths.at(i);
    const double scale = inner.width() / std::max(1.0, total);
    const double w = widths.value(index, 0.0) * scale;
    const double offset = start * scale;
    const double x = isRightToLeft() ? inner.right() - offset - w : inner.left() + offset;
    return {x, inner.top(), w, inner.height()};
}

int SegmentedControl::indexAt(const QPoint &pos) const
{
    for (int i = 0; i < count(); ++i)
        if (segmentRect(i).contains(pos))
            return i;
    return -1;
}

void SegmentedControl::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(Border, 1));
    p.setBrush(Background);
    p.drawRoundedRect(outer, outer.height() / 2, outer.height() / 2);

    // الكبسولة بين جزأين أثناء الحركة
    const int a = std::clamp(int(std::floor(m_position)), 0, count() - 1);
    const int b = std::clamp(a + 1, 0, count() - 1);
    const double t = m_position - a;
    const QRectF ra = segmentRect(a);
    const QRectF rb = segmentRect(b);
    const QRectF pill(ra.left() + (rb.left() - ra.left()) * t, ra.top(), ra.width() + (rb.width() - ra.width()) * t, ra.height());
    // كبسولة خضراء خافتة بحد أفتح، على نمط الألوان الناعمة
    p.setPen(QPen(isEnabled() ? withAlpha(Accent2, 70) : Border, 1));
    p.setBrush(isEnabled() ? mix(Surface, Accent, 0.38) : Border);
    p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    p.setPen(Qt::NoPen);

    for (int i = 0; i < count(); ++i) {
        const QRectF r = segmentRect(i);
        if (i == m_hovered && i != m_current) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(SurfaceHigh, 200));
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
        const double closeness = std::clamp(1.0 - std::abs(m_position - i), 0.0, 1.0);
        p.setPen(!isEnabled() ? Muted : mix(i == m_hovered ? Text : Muted, QColor(0xdc, 0xf5, 0xe6), closeness));
        QFont f = font();
        if (i == m_current)
            f.setWeight(QFont::DemiBold);
        p.setFont(f);
        // اتجاه النص من اتجاه العنصر، فتبقى «2×» كما هي في شريط لاتيني الاتجاه
        QTextOption option(Qt::AlignCenter);
        option.setTextDirection(layoutDirection());
        p.drawText(r, QFontMetrics(f).elidedText(m_items.at(i), Qt::ElideRight, int(r.width()) - 2), option);
    }
}

void SegmentedControl::mousePressEvent(QMouseEvent *event)
{
    const int index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index >= 0)
        setCurrentIndex(index);
}

void SegmentedControl::mouseMoveEvent(QMouseEvent *event)
{
    const int index = indexAt(event->position().toPoint());
    if (index != m_hovered) {
        m_hovered = index;
        update();
    }
}

void SegmentedControl::leaveEvent(QEvent *)
{
    m_hovered = -1;
    update();
}

bool SegmentedControl::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = indexAt(help->pos());
        const QString tip = index >= 0 && !m_tips.at(index).isEmpty() ? m_tips.at(index) : toolTip();
        if (tip.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(event);
}

// ───────────── Stepper ─────────────

Stepper::Stepper(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void Stepper::setValue(int value)
{
    value = std::clamp(value, m_minimum, m_maximum);
    if (value == m_value)
        return;
    m_value = value;
    update();
    emit valueChanged(value);
}

void Stepper::setRange(int minimum, int maximum)
{
    m_minimum = minimum;
    m_maximum = std::max(minimum, maximum);
    setValue(m_value);
    updateGeometry();
}

void Stepper::setSpecialValueText(const QString &text)
{
    m_special = text;
    updateGeometry();
    update();
}

void Stepper::setSuffix(const QString &suffix)
{
    m_suffix = suffix;
    updateGeometry();
    update();
}

QString Stepper::text() const
{
    if (m_value == m_minimum && !m_special.isEmpty())
        return m_special;
    return QLocale().toString(m_value) + m_suffix;
}

QSize Stepper::sizeHint() const
{
    const QFontMetrics fm = fontMetrics();
    const int text = std::max({fm.horizontalAdvance(m_special), fm.horizontalAdvance(QString::number(m_maximum) + m_suffix), 24});
    return {text + 16 + 2 * 30, 32};
}

// في الواجهة العربية «−» يمين القيمة و«+» يسارها، كاتجاه القراءة
QRectF Stepper::minusRect() const
{
    return isRightToLeft() ? QRectF(width() - 30, 0, 30, height()) : QRectF(0, 0, 30, height());
}

QRectF Stepper::plusRect() const
{
    return isRightToLeft() ? QRectF(0, 0, 30, height()) : QRectF(width() - 30, 0, 30, height());
}

void Stepper::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(hasFocus() ? Accent : Border, 1));
    p.setBrush(SurfaceHigh);
    p.drawRoundedRect(outer, 10, 10);

    const auto sign = [&](const QRectF &r, bool plus, bool enabled, bool hovered) {
        if (hovered && enabled) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(Accent, 60));
            p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 7, 7);
        }
        p.setPen(QPen(enabled ? (hovered ? Text : Muted) : withAlpha(Muted, 90), 1.8, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = r.center();
        p.drawLine(QPointF(c.x() - 5, c.y()), QPointF(c.x() + 5, c.y()));
        if (plus)
            p.drawLine(QPointF(c.x(), c.y() - 5), QPointF(c.x(), c.y() + 5));
    };
    const bool enabled = isEnabled();
    sign(minusRect(), false, enabled && m_value > m_minimum, m_hovered < 0);
    sign(plusRect(), true, enabled && m_value < m_maximum, m_hovered > 0);

    p.setPen(enabled ? Text : Muted);
    QFont f = font();
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.drawText(QRectF(rect()).adjusted(30, 0, -30, 0), Qt::AlignCenter, text());
}

void Stepper::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus(Qt::MouseFocusReason);
    if (minusRect().contains(event->position()))
        setValue(m_value - 1);
    else if (plusRect().contains(event->position()))
        setValue(m_value + 1);
}

void Stepper::mouseMoveEvent(QMouseEvent *event)
{
    const int hovered = minusRect().contains(event->position()) ? -1 : plusRect().contains(event->position()) ? 1 : 0;
    setCursor(hovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
}

void Stepper::leaveEvent(QEvent *)
{
    m_hovered = 0;
    update();
}

void Stepper::wheelEvent(QWheelEvent *event)
{
    setValue(m_value + (event->angleDelta().y() > 0 ? 1 : -1));
    event->accept();
}

void Stepper::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Plus:
        setValue(m_value + 1);
        break;
    case Qt::Key_Down:
    case Qt::Key_Minus:
        setValue(m_value - 1);
        break;
    case Qt::Key_Home:
        setValue(m_minimum);
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

// ───────────── ValueSlider ─────────────

ValueSlider::ValueSlider(QWidget *parent)
    : QSlider(Qt::Horizontal, parent)
    , m_formatter([](int value) { return QStringLiteral("%1%").arg(value); })
    , m_accent(Theme::Accent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    m_hoverAnimation = animation(this, 180, [this](double v) {
        m_hover = v;
        update();
    });
    connect(this, &QSlider::valueChanged, this, qOverload<>(&QWidget::update));
}

void ValueSlider::setFormatter(std::function<QString(int)> formatter)
{
    m_formatter = std::move(formatter);
    update();
}

void ValueSlider::setAccent(const QColor &color)
{
    m_accent = color;
    update();
}

QSize ValueSlider::sizeHint() const
{
    return {200, 46};
}

QSize ValueSlider::minimumSizeHint() const
{
    return {80, 46};
}

QRectF ValueSlider::grooveRect() const
{
    return {10, height() - 15.0, width() - 20.0, 4};
}

int ValueSlider::valueAt(double x) const
{
    const QRectF g = grooveRect();
    double t = std::clamp((x - g.left()) / g.width(), 0.0, 1.0);
    if (isRightToLeft())
        t = 1.0 - t;
    return minimum() + int(std::lround(t * (maximum() - minimum())));
}

void ValueSlider::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF g = grooveRect();
    const double t = maximum() > minimum() ? double(value() - minimum()) / (maximum() - minimum()) : 0.0;
    const double x = isRightToLeft() ? g.right() - t * g.width() : g.left() + t * g.width();
    const QColor accent = isEnabled() ? m_accent : Muted;

    p.setPen(Qt::NoPen);
    p.setBrush(Border);
    p.drawRoundedRect(g, 2, 2);
    const QRectF filled = isRightToLeft() ? QRectF(x, g.top(), g.right() - x, g.height()) : QRectF(g.left(), g.top(), x - g.left(), g.height());
    QLinearGradient gradient(filled.topLeft(), filled.topRight());
    gradient.setColorAt(0, isRightToLeft() ? accent : Accent2);
    gradient.setColorAt(1, isRightToLeft() ? Accent2 : accent);
    p.setBrush(gradient);
    p.drawRoundedRect(filled, 2, 2);

    // المقبض بهالة تكبر عند المرور
    const QPointF knob(x, g.center().y());
    p.setBrush(withAlpha(accent, int(70 + 50 * m_hover)));
    p.drawEllipse(knob, 9 + 3 * m_hover, 9 + 3 * m_hover);
    p.setBrush(Qt::white);
    p.drawEllipse(knob, 7, 7);

    // الفقاعة
    const QString text = m_formatter ? m_formatter(value()) : QString::number(value());
    QFont f = font();
    f.setPointSizeF(font().pointSizeF() * 0.9);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    const QFontMetrics fm(f);
    const double w = fm.horizontalAdvance(text) + 14;
    const double bx = std::clamp(x - w / 2.0, 0.0, width() - w);
    const QRectF bubble(bx, 0, w, 22);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(bubble, 7, 7);
    QPolygonF tip{QPointF(x - 4, bubble.bottom() - 0.5), QPointF(x + 4, bubble.bottom() - 0.5), QPointF(x, bubble.bottom() + 4)};
    p.drawPolygon(tip);
    p.setPen(Qt::white);
    p.drawText(bubble, Qt::AlignCenter, text);
}

void ValueSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setSliderDown(true);
    setValue(valueAt(event->position().x()));
}

void ValueSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        setValue(valueAt(event->position().x()));
}

void ValueSlider::enterEvent(QEnterEvent *)
{
    run(m_hoverAnimation, m_hover, 1.0);
}

void ValueSlider::leaveEvent(QEvent *)
{
    setSliderDown(false);
    run(m_hoverAnimation, m_hover, 0.0);
}

// ───────────── SelectBox ─────────────

SelectBox::SelectBox(QWidget *parent)
    : QComboBox(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
}

QSize SelectBox::sizeHint() const
{
    int widest = 40;
    for (int i = 0; i < count(); ++i)
        widest = std::max(widest, fontMetrics().horizontalAdvance(itemText(i)));
    return {widest + 44, 32};
}

QSize SelectBox::minimumSizeHint() const
{
    return {70, 32};
}

void SelectBox::showPopup()
{
    m_open = true;
    update();
    QComboBox::showPopup();
}

void SelectBox::hidePopup()
{
    m_open = false;
    update();
    QComboBox::hidePopup();
}

void SelectBox::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(m_open ? Accent : m_hovered ? mix(Border, Accent, 0.5) : Border, 1));
    p.setBrush(m_hovered ? mix(SurfaceHigh, Accent, 0.08) : SurfaceHigh);
    p.drawRoundedRect(outer, 10, 10);

    const bool rtl = isRightToLeft();
    const QRectF arrow = rtl ? QRectF(4, 0, 24, height()) : QRectF(width() - 28, 0, 24, height());
    paintChevron(p, arrow.center(), 8, m_open ? 180 : 0, isEnabled() ? Muted : withAlpha(Muted, 90));
    const QRectF textRect = rtl ? QRectF(28, 0, width() - 40, height()) : QRectF(12, 0, width() - 40, height());
    p.setPen(isEnabled() ? Text : Muted);
    p.drawText(textRect, (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter | Qt::AlignAbsolute,
               fontMetrics().elidedText(currentText(), Qt::ElideRight, int(textRect.width())));
}

void SelectBox::enterEvent(QEnterEvent *)
{
    m_hovered = true;
    update();
}

void SelectBox::leaveEvent(QEvent *)
{
    m_hovered = false;
    update();
}

// ───────────── CollapsibleSection ─────────────

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent, const QString &settingsKey)
    : QWidget(parent)
    , m_settingsKey(settingsKey)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->installEventFilter(this);
    auto *header = new QHBoxLayout(m_header);
    header->setContentsMargins(12, 8, 10, 8);
    header->setSpacing(8);
    m_title = new QLabel(title, m_header);
    m_title->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_chevron = new QLabel(m_header);
    m_chevron->setFixedSize(16, 16);
    header->addWidget(m_title);
    header->addStretch(1);
    header->addWidget(m_chevron);
    layout->addWidget(m_header);

    m_content = new QWidget(this);
    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(12, 4, 12, 12);
    m_contentLayout->setSpacing(10);
    layout->addWidget(m_content);

    m_animation = animation(this, 260, [this](double v) { applyHeight(v); });
    connect(m_animation, &QVariantAnimation::finished, this, [this] {
        // بعد الفتح يعود القسم إلى حجمه الطبيعي فيتبع تغيّر محتواه
        if (m_expanded)
            m_content->setMaximumHeight(QWIDGETSIZE_MAX);
        else
            m_content->setVisible(false);
    });

    if (!m_settingsKey.isEmpty())
        m_expanded = QSettings(AppSettings::settingsPath(), QSettings::IniFormat).value(m_settingsKey, true).toBool();
    setExpanded(m_expanded, false);
}

void CollapsibleSection::addHeaderWidget(QWidget *widget, int stretch)
{
    auto *header = static_cast<QHBoxLayout *>(m_header->layout());
    widget->setParent(m_header);
    widget->setCursor(Qt::ArrowCursor);
    // قبل السهم، وبعد العنوان والفراغ
    header->insertWidget(header->count() - 1, widget, stretch);
}

void CollapsibleSection::setTitle(const QString &title)
{
    m_title->setText(title);
}

void CollapsibleSection::setFlat(bool flat)
{
    m_flat = flat;
    m_header->layout()->setContentsMargins(flat ? QMargins(4, 4, 4, 4) : QMargins(12, 8, 10, 8));
    m_contentLayout->setContentsMargins(flat ? QMargins(0, 2, 0, 0) : QMargins(12, 4, 12, 12));
    update();
}

bool CollapsibleSection::savedExpanded() const
{
    if (m_settingsKey.isEmpty())
        return m_expanded;
    return QSettings(AppSettings::settingsPath(), QSettings::IniFormat).value(m_settingsKey, true).toBool();
}

void CollapsibleSection::setExpanded(bool expanded, bool animated, bool remember)
{
    const bool changed = expanded != m_expanded;
    m_expanded = expanded;
    if (!m_settingsKey.isEmpty() && changed && remember)
        QSettings(AppSettings::settingsPath(), QSettings::IniFormat).setValue(m_settingsKey, expanded);
    if (expanded)
        m_content->setVisible(true);
    if (animated && isVisible()) {
        run(m_animation, m_progress, expanded ? 1.0 : 0.0);
    } else {
        m_animation->stop();
        applyHeight(expanded ? 1.0 : 0.0);
        m_content->setMaximumHeight(QWIDGETSIZE_MAX);
        m_content->setVisible(expanded);
    }
    if (changed)
        emit expandedChanged(expanded);
}

void CollapsibleSection::applyHeight(double progress)
{
    m_progress = progress;
    const int full = std::max(m_content->sizeHint().height(), m_content->minimumSizeHint().height());
    // مفتوحاً بالكامل يكبر المحتوى بلا حد إن أُعطي مساحة (داخل مقسِّم مثلاً)
    m_content->setMaximumHeight(progress >= 1.0 ? QWIDGETSIZE_MAX : int(std::round(full * progress)));
    // القسم نفسه يتقلص أيضاً، فيستعيد المقسِّم الذي يحويه المساحة المحررة
    const QMargins margins = layout()->contentsMargins();
    setMaximumHeight(progress >= 1.0 ? QWIDGETSIZE_MAX
                                     : m_header->sizeHint().height() + int(std::round(full * progress)) + margins.top() + margins.bottom());
    // السهم يدور من «‹» إلى «⌄»
    const double angle = isRightToLeft() ? 90.0 * (1.0 - progress) : -90.0 * (1.0 - progress);
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    paintChevron(p, QPointF(8, 8), 8, angle, Theme::Muted);
    p.end();
    m_chevron->setPixmap(pixmap);
    update();
}

void CollapsibleSection::paintEvent(QPaintEvent *)
{
    if (m_flat)
        return;
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(Border, 1));
    p.setBrush(Surface);
    p.drawRoundedRect(outer, 12, 12);
    // خلفية العنوان أفتح، وتلتف زواياها السفلية حين يُطوى القسم
    QPainterPath clip;
    clip.addRoundedRect(outer, 12, 12);
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(SurfaceHigh, 160));
    p.drawRect(QRectF(0, 0, width(), m_header->geometry().bottom() + 1));
}

bool CollapsibleSection::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        // نقرة على عنصر مضاف إلى سطر العنوان لا تطوي القسم
        QWidget *child = m_header->childAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (child && child != m_title && child != m_chevron)
            return false;
        setExpanded(!m_expanded);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// ───────────── SideDrawer ─────────────

SideDrawer::SideDrawer(Icons::Name icon, const QString &title, QWidget *host)
    : QWidget(host)
{
    setFocusPolicy(Qt::StrongFocus);
    host->installEventFilter(this);

    m_panel = new QWidget(this);
    m_panel->setObjectName(QStringLiteral("drawerPanel"));
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->setStyleSheet(QStringLiteral("#drawerPanel { background: %1; border: 1px solid %2; border-radius: 16px; }")
                               .arg(Theme::Sidebar.name(), Theme::Border.name()));
    auto *panel = new QVBoxLayout(m_panel);
    panel->setContentsMargins(0, 0, 0, 0);
    panel->setSpacing(0);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(16, 14, 10, 10);
    header->setSpacing(8);
    auto *iconLabel = new QLabel(m_panel);
    iconLabel->setPixmap(Icons::icon(icon, Theme::Accent2, 20).pixmap(20, 20));
    auto *titleLabel = new QLabel(title, m_panel);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 700;"));
    auto *close = new QLabel(m_panel);
    close->setPixmap(Icons::icon(Icons::Name::Reject, Theme::Muted, 18).pixmap(18, 18));
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("إغلاق (Esc)"));
    close->setObjectName(QStringLiteral("drawerClose"));
    close->installEventFilter(this);
    header->addWidget(iconLabel);
    header->addWidget(titleLabel);
    header->addStretch(1);
    header->addWidget(close);
    panel->addLayout(header);

    auto *scroll = new QScrollArea(m_panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    auto *body = new QWidget(scroll);
    m_bodyLayout = new QVBoxLayout(body);
    m_bodyLayout->setContentsMargins(14, 4, 14, 10);
    m_bodyLayout->setSpacing(10);
    scroll->setWidget(body);
    panel->addWidget(scroll, 1);

    m_footerLayout = new QVBoxLayout;
    m_footerLayout->setContentsMargins(14, 8, 14, 14);
    m_footerLayout->setSpacing(8);
    panel->addLayout(m_footerLayout);

    m_animation = animation(this, 280, [this](double v) {
        m_progress = v;
        layoutPanel();
        update();
    });
    connect(m_animation, &QVariantAnimation::finished, this, [this] {
        if (!m_open)
            hide();
    });
    hide();
}

void SideDrawer::setOpen(bool open)
{
    if (open == m_open)
        return;
    m_open = open;
    if (open) {
        setGeometry(parentWidget()->rect());
        layoutPanel();
        show();
        raise();
        setFocus();
    }
    run(m_animation, m_progress, open ? 1.0 : 0.0);
    emit openChanged(open);
}

void SideDrawer::layoutPanel()
{
    const int width = std::min(m_panelWidth, this->width() - 40);
    const int margin = 10;
    // ينزلق من الطرف الأيسر في الواجهة العربية (طرف نهاية السطر حيث زر الإعدادات)
    const int shown = margin;
    const int hidden = -width - 20;
    const int offset = int(std::round(hidden + (shown - hidden) * m_progress));
    const int x = isRightToLeft() ? offset : this->width() - width - offset;
    m_panel->setGeometry(x, margin, width, height() - 2 * margin);
}

void SideDrawer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(2, 6, 14, int(140 * m_progress)));
}

void SideDrawer::mousePressEvent(QMouseEvent *event)
{
    if (!m_panel->geometry().contains(event->position().toPoint()))
        setOpen(false);
}

void SideDrawer::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
        setOpen(false);
    else
        QWidget::keyPressEvent(event);
}

bool SideDrawer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        setGeometry(parentWidget()->rect());
        layoutPanel();
    } else if (watched->objectName() == QLatin1String("drawerClose") && event->type() == QEvent::MouseButtonRelease) {
        setOpen(false);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// ───────────── ProgressRing ─────────────

ProgressRing::ProgressRing(QWidget *parent)
    : QWidget(parent)
    , m_color(Theme::Accent2)
{
    setFixedSize(26, 26);
    m_spinAnimation = new QVariantAnimation(this);
    m_spinAnimation->setStartValue(0.0);
    m_spinAnimation->setEndValue(360.0);
    m_spinAnimation->setDuration(1200);
    m_spinAnimation->setLoopCount(-1);
    connect(m_spinAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_spin = v.toDouble();
        update();
    });
}

void ProgressRing::setProgress(double progress)
{
    m_progress = progress < 0.0 ? -1.0 : std::clamp(progress, 0.0, 1.0);
    if (m_progress < 0.0 && m_spinAnimation->state() != QAbstractAnimation::Running)
        m_spinAnimation->start();
    else if (m_progress >= 0.0)
        m_spinAnimation->stop();
    update();
}

void ProgressRing::setIcon(Icons::Name icon, const QColor &color)
{
    m_progress = -2.0;
    m_spinAnimation->stop();
    m_icon = icon;
    m_color = color;
    update();
}

void ProgressRing::paintEvent(QPaintEvent *)
{
    using namespace Theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_progress <= -2.0) {
        Icons::paint(p, m_icon, QRectF(3, 3, 20, 20), m_color);
        return;
    }
    const QRectF ring = QRectF(rect()).adjusted(3, 3, -3, -3);
    p.setPen(QPen(Border, 3));
    p.drawEllipse(ring);
    p.setPen(QPen(Accent2, 3, Qt::SolidLine, Qt::RoundCap));
    if (m_progress < 0.0)
        p.drawArc(ring, int(-m_spin * 16), 90 * 16);
    else
        p.drawArc(ring, 90 * 16, int(-m_progress * 360 * 16));
}

FloatingBar::FloatingBar(QWidget *host, bool atEnd)
    : QWidget(host)
    , m_layout(new QHBoxLayout(this))
    , m_atEnd(atEnd)
{
    m_layout->setContentsMargins(6, 4, 6, 4);
    m_layout->setSpacing(2);
    // الأدوات تُقرأ من اليسار مثل «− 100% +» في كل اللغات؛ الموضع وحده يتبع اتجاه الواجهة
    setLayoutDirection(Qt::LeftToRight);
    setStyleSheet(QStringLiteral("QToolButton { background: transparent; border: none; border-radius: 13px; padding: 0; }"
                                 "QToolButton:hover { background: %1; }"
                                 "QToolButton:disabled { background: transparent; }"
                                 "QLabel { color: %2; font-size: 9pt; padding: 0 4px; }")
                      .arg(Theme::SurfaceHigh.name(), Theme::Text.name()));
    host->installEventFilter(this);
}

QToolButton *FloatingBar::addButton(Icons::Name icon, const QString &tip)
{
    auto *button = new QToolButton(this);
    button->setIcon(Icons::icon(icon, Theme::Text, 16));
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(26, 26);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    m_layout->addWidget(button);
    return button;
}

void FloatingBar::reposition()
{
    QWidget *host = parentWidget();
    if (!host)
        return;
    adjustSize();
    const int margin = 12;
    int x = (host->width() - width()) / 2;
    if (m_atEnd)
        x = host->isRightToLeft() ? margin : host->width() - width() - margin;
    move(x, host->height() - height() - margin);
    raise();
}

bool FloatingBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        reposition();
    return QWidget::eventFilter(watched, event);
}

void FloatingBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF bar = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(Theme::withAlpha(Theme::Border, 230), 1));
    p.setBrush(Theme::withAlpha(Theme::Surface, 225));
    p.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
}

LiveBadge::LiveBadge(QWidget *parent)
    : QWidget(parent)
    , m_animation(new QVariantAnimation(this))
{
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    m_animation->setDuration(1400);
    m_animation->setLoopCount(-1);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_pulse = value.toDouble();
        update();
    });
    setToolTip(QStringLiteral("بث حي من الكاميرا"));
}

QSize LiveBadge::sizeHint() const
{
    QFont bold = font();
    bold.setWeight(QFont::Bold);
    return {QFontMetrics(bold).horizontalAdvance(QStringLiteral("مباشر")) + 40, 30};
}

void LiveBadge::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_animation->start();
}

void LiveBadge::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_animation->stop();
}

void LiveBadge::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF pill = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::withAlpha(Theme::Danger, 46));
    p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);

    // النقطة وموجة تتسع حولها وتخفت
    const bool rtl = isRightToLeft();
    const QPointF dot(rtl ? pill.right() - 15 : pill.left() + 15, pill.center().y());
    p.setBrush(Theme::withAlpha(Theme::Danger, int(150 * (1.0 - m_pulse))));
    p.drawEllipse(dot, 4 + 6 * m_pulse, 4 + 6 * m_pulse);
    p.setBrush(QColor(0xff, 0x6b, 0x45));
    p.drawEllipse(dot, 4.5, 4.5);

    QFont bold = font();
    bold.setWeight(QFont::Bold);
    p.setFont(bold);
    p.setPen(QColor(0xff, 0xb4, 0x9c));
    const QRectF text = rtl ? pill.adjusted(8, 0, -28, 0) : pill.adjusted(28, 0, -8, 0);
    p.drawText(text, Qt::AlignCenter, QStringLiteral("مباشر"));
}

// ───────────── نص مختصر ─────────────

ElidedLabel::ElidedLabel(QWidget *parent)
    : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::setFullText(const QString &text)
{
    if (text == m_fullText)
        return;
    m_fullText = text;
    setToolTip(text);
    refresh();
    updateGeometry();
}

QSize ElidedLabel::minimumSizeHint() const
{
    return {fontMetrics().horizontalAdvance(QStringLiteral("……")), QLabel::minimumSizeHint().height()};
}

QSize ElidedLabel::sizeHint() const
{
    return {fontMetrics().horizontalAdvance(m_fullText) + 2, QLabel::sizeHint().height()};
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    refresh();
}

void ElidedLabel::refresh()
{
    const QString shown = fontMetrics().elidedText(m_fullText, Qt::ElideRight, std::max(0, contentsRect().width()));
    if (shown != text())
        setText(shown);
}
