#include "imageview.h"

#include "icons.h"
#include "theme.h"

#include <QEasingCurve>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

double easeOutBack(double t)
{
    return QEasingCurve(QEasingCurve::OutBack).valueForProgress(std::clamp(t, 0.0, 1.0));
}

QRectF lerp(const QRectF &a, const QRectF &b, double t)
{
    return QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                  a.width() + (b.width() - a.width()) * t, a.height() + (b.height() - a.height()) * t);
}

} // namespace

ImageView::ImageView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &ImageView::tick);
    m_clock.start();
    startTicking();
}

void ImageView::setImage(const QImage &image, bool animate)
{
    if (animate && !m_image.isNull()) {
        m_previous = m_image;
        m_fade = 0.0;
    } else if (animate) {
        m_previous = QImage();
        m_fade = 0.0;
    } else {
        m_previous = QImage();
        m_fade = 1.0;
    }
    m_image = image;
    startTicking();
    update();
}

void ImageView::setFaces(const QList<FaceItem> &faces)
{
    QList<Box> boxes;
    int newCount = 0;
    for (const FaceItem &face : faces) {
        QString label = matchLabel(face.match);
        if (face.match.status != MatchStatus::Unknown)
            label += QStringLiteral("  %1%").arg(qRound(face.match.score * 100));
        Box box{face.id, face.box, face.box, label, matchColor(face.match.status), 0.0};
        const auto existing = std::find_if(m_boxes.begin(), m_boxes.end(), [&](const Box &b) { return b.id == face.id; });
        if (existing != m_boxes.end()) {
            box.current = existing->current;
            box.appear = existing->appear;
        } else {
            box.appear = -0.14 * newCount++;
        }
        boxes.append(box);
    }
    m_boxes = boxes;
    startTicking();
    update();
}

void ImageView::setSelectedFace(int id)
{
    m_selected = id;
    startTicking();
    update();
}

void ImageView::setBusy(bool busy, const QString &text)
{
    m_busy = busy;
    m_busyText = text;
    startTicking();
    update();
}

void ImageView::clear()
{
    m_image = QImage();
    m_previous = QImage();
    m_boxes.clear();
    m_selected = -1;
    m_hovered = -1;
    startTicking();
    update();
}

void ImageView::startTicking()
{
    if (!m_timer.isActive())
        m_timer.start();
}

void ImageView::tick()
{
    bool active = m_busy || m_image.isNull() || m_selected >= 0;
    if (m_fade < 1.0) {
        m_fade = std::min(1.0, m_fade + 0.06);
        active = true;
    }
    for (Box &box : m_boxes) {
        if (box.appear < 1.0) {
            box.appear = std::min(1.0, box.appear + 0.055);
            active = true;
        }
        const QRectF next = lerp(box.current, box.target, 0.35);
        if (std::abs(next.x() - box.target.x()) + std::abs(next.width() - box.target.width()) > 0.3) {
            box.current = next;
            active = true;
        } else {
            box.current = box.target;
        }
    }
    update();
    if (!active)
        m_timer.stop();
}

QRectF ImageView::imageRect(const QImage &image) const
{
    if (image.isNull())
        return {};
    QSizeF size = image.size();
    size.scale(QSizeF(this->size()).shrunkBy(QMarginsF(12, 12, 12, 12)), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
}

QRectF ImageView::mapToView(const QRectF &rect) const
{
    const QRectF target = imageRect(m_image);
    const double scale = target.width() / m_image.width();
    return QRectF(target.x() + rect.x() * scale, target.y() + rect.y() * scale,
                  rect.width() * scale, rect.height() * scale);
}

int ImageView::boxAt(const QPointF &pos) const
{
    for (const Box &box : m_boxes) {
        if (mapToView(box.current).contains(pos))
            return box.id;
    }
    return -1;
}

void ImageView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double t = m_clock.elapsed() / 1000.0;

    if (m_image.isNull()) {
        paintEmpty(p, t);
        return;
    }

    // انتقال متلاشٍ بين الصورة السابقة والجديدة مع تكبير خفيف
    const double fade = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(m_fade);
    if (!m_previous.isNull() && fade < 1.0) {
        p.setOpacity(1.0 - fade);
        p.drawImage(imageRect(m_previous), m_previous);
    }
    p.setOpacity(fade);
    const QRectF target = imageRect(m_image);
    const double zoom = 1.0 + 0.03 * (1.0 - fade);
    const QRectF zoomed(target.center() - QPointF(target.width() * zoom / 2, target.height() * zoom / 2),
                        target.size() * zoom);
    QPainterPath clip;
    clip.addRoundedRect(target, 10, 10);
    p.setClipPath(clip);
    p.drawImage(zoomed, m_image);
    p.setClipping(false);
    p.setOpacity(1.0);

    for (const Box &box : m_boxes)
        paintBox(p, box, t);

    if (m_busy)
        paintBusy(p, t);
}

void ImageView::paintBox(QPainter &p, const Box &box, double t)
{
    using namespace Theme;
    const double appear = std::clamp(box.appear, 0.0, 1.0);
    if (appear <= 0.0)
        return;
    const double alpha = std::min(1.0, appear * 1.6);
    const double scale = 1.35 - 0.35 * easeOutBack(appear);
    const QRectF base = mapToView(box.current);
    const QRectF r(base.center() - QPointF(base.width() * scale / 2, base.height() * scale / 2), base.size() * scale);
    const bool selected = box.id == m_selected;
    const bool hovered = box.id == m_hovered;

    // توهج نابض للوجه المحدد
    const double pulse = selected ? 0.5 + 0.5 * std::sin(t * 4.0) : 0.0;
    p.setBrush(withAlpha(box.color, int((hovered || selected ? 34 : 14) * alpha)));
    p.setPen(QPen(withAlpha(box.color, int((selected ? 70 + 60 * pulse : 45) * alpha)), selected ? 7 + 3 * pulse : 5));
    p.drawRoundedRect(r, 8, 8);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(withAlpha(box.color, int(110 * alpha)), 1.2));
    p.drawRoundedRect(r, 8, 8);

    // زوايا بارزة
    const double len = std::min(r.width(), r.height()) * 0.24;
    p.setPen(QPen(withAlpha(box.color.lighter(120), int(255 * alpha)), 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    auto corner = [&p, len](QPointF c, double dx, double dy) {
        QPainterPath path;
        path.moveTo(c.x() + dx * len, c.y());
        path.lineTo(c);
        path.lineTo(c.x(), c.y() + dy * len);
        p.drawPath(path);
    };
    corner(r.topLeft(), 1, 1);
    corner(r.topRight(), -1, 1);
    corner(r.bottomLeft(), 1, -1);
    corner(r.bottomRight(), -1, -1);

    // بطاقة الاسم
    QFont font = this->font();
    font.setPointSizeF(10);
    font.setWeight(QFont::Bold);
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const QSizeF size(metrics.horizontalAdvance(box.label) + 22, 26);
    const double lift = (1.0 - easeOutBack(appear)) * 10.0;
    QRectF pill(QPointF(r.center().x() - size.width() / 2, r.top() - size.height() - 8 - lift), size);
    if (pill.top() < 4)
        pill.moveTop(r.bottom() + 8 + lift);
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(box.color.darker(110), int(235 * alpha)));
    p.drawRoundedRect(pill, 13, 13);
    p.setPen(withAlpha(Qt::white, int(255 * alpha)));
    p.drawText(pill, Qt::AlignCenter, box.label);
}

void ImageView::paintEmpty(QPainter &p, double t)
{
    using namespace Theme;
    const QPointF c(width() / 2.0, height() / 2.0 - 30);

    // حلقات تتسع وتتلاشى بشكل متكرر
    for (int k = 0; k < 3; ++k) {
        const double phase = std::fmod(t / 2.4 + k / 3.0, 1.0);
        const double radius = 48 + 90 * phase;
        p.setPen(QPen(withAlpha(mix(Accent, Accent2, phase), int(120 * (1.0 - phase))), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, radius, radius);
    }
    const double breathe = 1.0 + 0.04 * std::sin(t * 2.2);
    QRadialGradient glow(c, 90);
    glow.setColorAt(0, withAlpha(Accent, 70));
    glow.setColorAt(1, withAlpha(Accent, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(c, 90, 90);
    QLinearGradient fill(c - QPointF(44, 44), c + QPointF(44, 44));
    fill.setColorAt(0, Accent);
    fill.setColorAt(1, Accent2);
    p.setBrush(fill);
    p.drawEllipse(c, 44 * breathe, 44 * breathe);
    Icons::paint(p, Icons::Name::Scan, QRectF(c - QPointF(22, 22), QSizeF(44, 44)), Qt::white);

    QFont title = font();
    title.setPointSizeF(15);
    title.setWeight(QFont::Bold);
    p.setFont(title);
    p.setPen(Text);
    p.drawText(QRectF(0, c.y() + 150, width(), 30), Qt::AlignCenter, QStringLiteral("ابدأ بفتح صورة أو فيديو أو تشغيل الكاميرا"));
    QFont sub = font();
    sub.setPointSizeF(10);
    p.setFont(sub);
    p.setPen(Muted);
    p.drawText(QRectF(0, c.y() + 184, width(), 24), Qt::AlignCenter,
               QStringLiteral("كشف الوجوه والتعرف عليها يتم بالكامل على هذا الجهاز دون إنترنت"));
}

void ImageView::paintBusy(QPainter &p, double t)
{
    using namespace Theme;
    const QRectF r = imageRect(m_image);
    QPainterPath clip;
    clip.addRoundedRect(r, 10, 10);
    p.save();
    p.setClipPath(clip);
    p.fillRect(r, withAlpha(Background, 120));

    // خط مسح يتحرك ذهاباً وإياباً مع ذيل متدرج
    const double raw = std::fmod(t / 1.5, 2.0);
    const double phase = QEasingCurve(QEasingCurve::InOutSine).valueForProgress(raw < 1.0 ? raw : 2.0 - raw);
    const bool down = raw < 1.0;
    const double y = r.top() + phase * r.height();
    QLinearGradient trail(0, y, 0, down ? y - 110 : y + 110);
    trail.setColorAt(0, withAlpha(Accent2, 110));
    trail.setColorAt(1, withAlpha(Accent2, 0));
    p.fillRect(QRectF(r.left(), down ? y - 110 : y, r.width(), 110), trail);
    p.setPen(QPen(withAlpha(Accent2, 230), 2));
    p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    p.restore();

    // بطاقة الحالة مع نقاط متحركة
    QFont font = this->font();
    font.setPointSizeF(11);
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    const QString text = m_busyText.isEmpty() ? QStringLiteral("جارٍ التحليل") : m_busyText;
    const QSizeF size(QFontMetricsF(font).horizontalAdvance(text) + 90, 44);
    const QRectF pill(QPointF(r.center().x() - size.width() / 2, r.center().y() - size.height() / 2), size);
    p.setPen(QPen(withAlpha(Accent, 160), 1));
    p.setBrush(withAlpha(QColor(0x0e, 0x1d, 0x33), 235));
    p.drawRoundedRect(pill, 22, 22);
    p.setPen(Text);
    const bool rtl = isRightToLeft();
    p.drawText(pill.adjusted(rtl ? 60 : 22, 0, rtl ? -22 : -60, 0),
               (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter | Qt::AlignAbsolute, text);
    for (int i = 0; i < 3; ++i) {
        const double bounce = std::max(0.0, std::sin(t * 6.0 - i * 0.7));
        const double x = rtl ? pill.left() + 22 + i * 12 : pill.right() - 46 + i * 12;
        p.setPen(Qt::NoPen);
        p.setBrush(mix(Accent, Accent2, i / 2.0));
        p.drawEllipse(QPointF(x, pill.center().y() - 5 * bounce), 3.5, 3.5);
    }
}

void ImageView::mousePressEvent(QMouseEvent *event)
{
    const int id = boxAt(event->position());
    if (id >= 0) {
        emit faceClicked(id);
        return;
    }
    QWidget::mousePressEvent(event);
}

void ImageView::mouseMoveEvent(QMouseEvent *event)
{
    const int id = boxAt(event->position());
    if (id != m_hovered) {
        m_hovered = id;
        setCursor(id >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void ImageView::leaveEvent(QEvent *event)
{
    m_hovered = -1;
    update();
    QWidget::leaveEvent(event);
}
