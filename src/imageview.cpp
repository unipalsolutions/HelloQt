#include "imageview.h"

#include "icons.h"
#include "theme.h"

#include <QEasingCurve>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

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
    // إطارات الفيديو المتتالية تحتفظ بالتكبير، أما صورة بحجم مختلف فتبدأ كاملة
    if (image.size() != m_image.size()) {
        m_zoom = 1.0;
        m_pan = QPointF();
    }
    // صيغة يرسمها Qt مباشرة؛ إطارات BGR888 كانت تُحوَّل في كل رسم
    m_image = image.format() == QImage::Format_ARGB32_Premultiplied || image.format() == QImage::Format_RGB32
        ? image : image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    m_scaled = QImage();
    startTicking();
    update();
}

void ImageView::setWatched(const QSet<QString> &personIds)
{
    if (personIds == m_watched)
        return;
    m_watched = personIds;
    setFaces(m_faces);
}

void ImageView::setFaces(const QList<FaceItem> &faces)
{
    m_faces = faces;
    QList<Box> boxes;
    int newCount = 0;
    for (const FaceItem &face : faces) {
        QString label = matchLabel(face.match);
        if (face.match.status != MatchStatus::Unknown)
            label += QStringLiteral("  %1%").arg(qRound(face.match.score * 100));
        const bool watched = face.match.status == MatchStatus::Confirmed && m_watched.contains(face.match.personId);
        if (watched)
            label = QStringLiteral("⚠  ") + label;
        Box box{face.id, face.box, face.box, label, watched ? Theme::Accent : matchColor(face.match.status), 0.0, watched};
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
    m_scaled = QImage();
    m_boxes.clear();
    m_selected = -1;
    m_hovered = -1;
    startTicking();
    update();
}

void ImageView::startTicking()
{
    m_timer.setInterval(16);
    if (!m_timer.isActive() && isVisible())
        m_timer.start();
}

void ImageView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    startTicking();
}

void ImageView::tick()
{
    // مخفي (صفحة أخرى أو نافذة مصغّرة): لا رسم، ويعود المؤقت عند الظهور
    if (!isVisible()) {
        m_timer.stop();
        return;
    }
    // حركة مستمرة هادئة (النبض والانتظار وشاشة البداية) تكفيها 30 إطاراً في الثانية، والانتقالات 60
    const bool ambient = m_busy || m_image.isNull() || m_selected >= 0
                         || std::any_of(m_boxes.cbegin(), m_boxes.cend(), [](const Box &b) { return b.watched; });
    bool active = false;
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
    if (!active && !ambient)
        m_timer.stop();
    else
        m_timer.setInterval(active ? 16 : 33);
}

QRectF ImageView::fitRect(const QImage &image) const
{
    if (image.isNull())
        return {};
    QSizeF size = image.size();
    size.scale(QSizeF(this->size()).shrunkBy(QMarginsF(12, 12, 12, 12)), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
}

QRectF ImageView::imageRect() const
{
    const QRectF fit = fitRect(m_image);
    if (fit.isEmpty() || m_zoom <= 1.0)
        return fit;
    const QSizeF size = fit.size() * m_zoom;
    const QPointF center = fit.center() + m_pan;
    return QRectF(center - QPointF(size.width() / 2, size.height() / 2), size);
}

void ImageView::setZoom(double zoom, const QPointF &anchor)
{
    if (m_image.isNull())
        return;
    zoom = std::clamp(zoom, 1.0, 12.0);
    const QRectF before = imageRect();
    const QPointF point = anchor.x() < 0 ? QPointF(width() / 2.0, height() / 2.0) : anchor;
    // النقطة التي تحت المؤشر تبقى تحته بعد التكبير
    const QPointF fraction((point.x() - before.x()) / before.width(), (point.y() - before.y()) / before.height());
    const QRectF fit = fitRect(m_image);
    const QSizeF size = fit.size() * zoom;
    const QPointF topLeft(point.x() - fraction.x() * size.width(), point.y() - fraction.y() * size.height());
    m_zoom = zoom;
    m_pan = topLeft + QPointF(size.width() / 2, size.height() / 2) - fit.center();
    clampPan();
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::clampPan()
{
    const QRectF fit = fitRect(m_image);
    if (fit.isEmpty() || m_zoom <= 1.0) {
        m_pan = QPointF();
        return;
    }
    const QSizeF size = fit.size() * m_zoom;
    const double maxX = std::max(0.0, (size.width() - (width() - 12)) / 2.0);
    const double maxY = std::max(0.0, (size.height() - (height() - 12)) / 2.0);
    m_pan.setX(std::clamp(m_pan.x(), -maxX, maxX));
    m_pan.setY(std::clamp(m_pan.y(), -maxY, maxY));
}

void ImageView::zoomBy(double factor, const QPointF &anchor)
{
    setZoom(m_zoom * factor, anchor);
}

void ImageView::resetZoom()
{
    if (m_zoom == 1.0 && m_pan.isNull())
        return;
    m_zoom = 1.0;
    m_pan = QPointF();
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::zoomToFace(int id)
{
    const auto box = std::find_if(m_boxes.cbegin(), m_boxes.cend(), [id](const Box &b) { return b.id == id; });
    const QRectF fit = fitRect(m_image);
    if (box == m_boxes.cend() || fit.isEmpty() || box->target.isEmpty())
        return;
    // الوجه يملأ نحو 45% من ارتفاع العرض ويتوسطه
    const double fitScale = fit.width() / m_image.width();
    m_zoom = std::clamp(0.45 * height() / (box->target.height() * fitScale), 1.0, 8.0);
    const double scale = fitScale * m_zoom;
    const QPointF face = box->target.center() - QPointF(m_image.width() / 2.0, m_image.height() / 2.0);
    m_pan = -face * scale;
    clampPan();
    emit zoomChanged(m_zoom);
    update();
}

void ImageView::setLabelsVisible(bool visible)
{
    m_showLabels = visible;
    update();
}

void ImageView::setNavigation(bool canPrevious, bool canNext, const QString &caption)
{
    m_canPrevious = canPrevious;
    m_canNext = canNext;
    m_caption = caption;
    update();
}

QRectF ImageView::navButton(int step) const
{
    if (!m_canPrevious && !m_canNext)
        return {};
    // في الواجهة العربية تأتي الصورة التالية من اليسار
    const bool left = (step > 0) == isRightToLeft();
    const double x = left ? 14 : width() - 14 - 44;
    return QRectF(x, height() / 2.0 - 22, 44, 44);
}

void ImageView::paintNavigation(QPainter &p)
{
    using namespace Theme;
    if (!m_canPrevious && !m_canNext)
        return;
    p.save();
    for (int step : {-1, 1}) {
        const bool enabled = step < 0 ? m_canPrevious : m_canNext;
        if (!enabled)
            continue;
        const QRectF r = navButton(step);
        const bool hovered = m_navHover == step;
        p.setPen(QPen(withAlpha(Accent, hovered ? 220 : 90), 1));
        p.setBrush(withAlpha(QColor(0x0e, 0x1d, 0x33), hovered ? 240 : 170));
        p.drawEllipse(r);
        // سهم يشير إلى جهة الصورة
        const bool pointLeft = r.center().x() < width() / 2.0;
        const QPointF c = r.center();
        const double d = pointLeft ? 1 : -1;
        QPainterPath arrow;
        arrow.moveTo(c.x() + 4 * d, c.y() - 8);
        arrow.lineTo(c.x() - 4 * d, c.y());
        arrow.lineTo(c.x() + 4 * d, c.y() + 8);
        p.setPen(QPen(withAlpha(Qt::white, hovered ? 255 : 200), 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(arrow);
    }
    if (!m_caption.isEmpty()) {
        QFont font = this->font();
        font.setPointSizeF(9.5);
        p.setFont(font);
        const QFontMetricsF metrics(font);
        const QString text = metrics.elidedText(m_caption, Qt::ElideMiddle, std::max(80, width() - 160));
        const QSizeF size(metrics.horizontalAdvance(text) + 28, 28);
        const QRectF pill(QPointF((width() - size.width()) / 2.0, height() - size.height() - 12), size);
        p.setPen(QPen(withAlpha(Accent, 90), 1));
        p.setBrush(withAlpha(QColor(0x0e, 0x1d, 0x33), 215));
        p.drawRoundedRect(pill, 14, 14);
        p.setPen(Text);
        p.drawText(pill, Qt::AlignCenter, text);
    }
    p.restore();
}

void ImageView::paintZoomBadge(QPainter &p)
{
    using namespace Theme;
    if (m_zoom <= 1.001 || m_image.isNull())
        return;
    p.save();
    QFont font = this->font();
    font.setPointSizeF(9.5);
    p.setFont(font);
    // \u2066…\u2069 تُبقي النسبة بترتيبها الصحيح داخل النص العربي
    const QString text = QStringLiteral("\u2066%1%\u2069  ·  نقر مزدوج للعرض الكامل").arg(qRound(m_zoom * 100));
    const QSizeF size(QFontMetricsF(font).horizontalAdvance(text) + 28, 28);
    const QRectF pill(QPointF(isRightToLeft() ? 12 : width() - size.width() - 12, 12), size);
    p.setPen(QPen(withAlpha(Accent2, 140), 1));
    p.setBrush(withAlpha(QColor(0x0e, 0x1d, 0x33), 225));
    p.drawRoundedRect(pill, 14, 14);
    p.setPen(Text);
    p.drawText(pill, Qt::AlignCenter, text);
    p.restore();
}

QImage ImageView::annotatedImage() const
{
    if (m_image.isNull())
        return {};
    QImage out = m_image.convertToFormat(QImage::Format_RGB32);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    // سماكة الخطوط والخط تتناسب مع حجم الصورة حتى تُقرأ في الصور الكبيرة والصغيرة
    const double unit = std::max(1.0, std::max(out.width(), out.height()) / 1000.0);
    QFont font = this->font();
    font.setPixelSize(int(std::lround(15 * unit)));
    font.setWeight(QFont::Bold);
    p.setFont(font);
    const QFontMetricsF metrics(font);
    for (const Box &box : m_boxes) {
        const QRectF r = box.target;
        p.setPen(QPen(box.color, 3 * unit));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 6 * unit, 6 * unit);
        const QSizeF size(metrics.horizontalAdvance(box.label) + 18 * unit, metrics.height() + 8 * unit);
        QRectF pill(QPointF(r.center().x() - size.width() / 2, r.top() - size.height() - 4 * unit), size);
        if (pill.top() < 0)
            pill.moveTop(r.bottom() + 4 * unit);
        pill.moveLeft(std::clamp(pill.left(), 0.0, std::max(0.0, out.width() - pill.width())));
        p.setPen(Qt::NoPen);
        p.setBrush(box.color.darker(115));
        p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, box.label);
    }
    return out;
}

QRectF ImageView::mapToView(const QRectF &rect) const
{
    const QRectF target = imageRect();
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
        p.drawImage(fitRect(m_previous), m_previous);
    }
    p.setOpacity(fade);
    const QRectF target = imageRect();
    const double zoom = 1.0 + 0.03 * (1.0 - fade);
    const QRectF zoomed(target.center() - QPointF(target.width() * zoom / 2, target.height() * zoom / 2),
                        target.size() * zoom);
    QPainterPath clip;
    clip.addRoundedRect(target.intersected(QRectF(rect()).adjusted(6, 6, -6, -6)), 10, 10);
    p.setClipPath(clip);
    if (fade >= 1.0 && m_zoom <= 1.0) {
        // بلا تكبير ولا انتقال: نسخة مصغّرة مرة واحدة لكل إطار وحجم نافذة
        const QSize size = target.size().toSize();
        if (m_scaled.size() != size || m_scaledKey != m_image.cacheKey()) {
            m_scaled = m_image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            m_scaledKey = m_image.cacheKey();
        }
        p.drawImage(target.topLeft(), m_scaled);
    } else {
        p.drawImage(zoomed, m_image);
    }
    p.setOpacity(1.0);

    paintRegion(p);
    for (const Box &box : m_boxes)
        paintBox(p, box, t);
    p.setClipping(false);

    if (m_busy)
        paintBusy(p, t);
    paintZoomBadge(p);
    paintNavigation(p);
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
    const double pulse = selected || box.watched ? 0.5 + 0.5 * std::sin(t * (box.watched ? 6.0 : 4.0)) : 0.0;
    p.setBrush(withAlpha(box.color, int((hovered || selected ? 34 : 14) * alpha)));
    const bool glow = selected || box.watched;
    p.setPen(QPen(withAlpha(box.watched ? Accent2 : box.color, int((glow ? 70 + 60 * pulse : 45) * alpha)),
                  glow ? 7 + (box.watched ? 6 : 3) * pulse : 5));
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

    // بطاقة الاسم: عند إخفاء الأسماء تظهر للوجه المحدد أو الذي تحت المؤشر فقط
    if (!m_showLabels && !selected && !hovered && !box.watched)
        return;
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
    const QRectF r = imageRect();
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

void ImageView::setRegion(const QRectF &region)
{
    m_region = region.normalized().intersected(QRectF(0, 0, 1, 1));
    update();
}

void ImageView::setSelectingRegion(bool selecting)
{
    m_selecting = selecting;
    m_dragging = false;
    setCursor(selecting ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

QPointF ImageView::toImageFraction(const QPointF &pos) const
{
    const QRectF target = imageRect();
    if (target.isEmpty())
        return {};
    return QPointF(std::clamp((pos.x() - target.x()) / target.width(), 0.0, 1.0),
                   std::clamp((pos.y() - target.y()) / target.height(), 0.0, 1.0));
}

void ImageView::paintRegion(QPainter &p)
{
    using namespace Theme;
    const QRectF target = imageRect();
    QRectF fraction = m_region;
    if (m_dragging)
        fraction = QRectF(m_dragStart, m_dragEnd).normalized();
    if (fraction.isEmpty() || target.isEmpty()) {
        if (m_selecting && !m_dragging) {
            // تلميح داخل الصورة حتى يعرف المستخدم أن عليه السحب
            p.save();
            QFont font = this->font();
            font.setWeight(QFont::DemiBold);
            p.setFont(font);
            const QString text = QStringLiteral("اسحب بالفأرة لتحديد المنطقة التي يُبحث فيها عن الوجوه");
            const QSizeF size(QFontMetricsF(font).horizontalAdvance(text) + 36, 36);
            const QRectF pill(QPointF(target.center().x() - size.width() / 2, target.top() + 14), size);
            p.setPen(QPen(withAlpha(Accent2, 200), 1));
            p.setBrush(withAlpha(QColor(0x0e, 0x1d, 0x33), 230));
            p.drawRoundedRect(pill, 18, 18);
            p.setPen(Text);
            p.drawText(pill, Qt::AlignCenter, text);
            p.restore();
        }
        return;
    }
    const QRectF r(target.x() + fraction.x() * target.width(), target.y() + fraction.y() * target.height(),
                   fraction.width() * target.width(), fraction.height() * target.height());
    p.save();
    QPainterPath outside;
    outside.addRoundedRect(target, 10, 10);
    QPainterPath inside;
    inside.addRect(r);
    p.fillPath(outside.subtracted(inside), withAlpha(Background, 150));
    QPen pen(Accent2, 2, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    p.setPen(Qt::NoPen);
    p.setBrush(Accent2);
    for (const QPointF &corner : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()})
        p.drawEllipse(corner, 4, 4);
    p.restore();
}

void ImageView::mousePressEvent(QMouseEvent *event)
{
    if (m_selecting && event->button() == Qt::LeftButton && !m_image.isNull()) {
        m_dragging = true;
        m_dragStart = m_dragEnd = toImageFraction(event->position());
        update();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        for (int step : {-1, 1}) {
            if ((step < 0 ? m_canPrevious : m_canNext) && navButton(step).contains(event->position())) {
                emit navigateRequested(step);
                return;
            }
        }
    }
    const int id = event->button() == Qt::LeftButton ? boxAt(event->position()) : -1;
    if (id >= 0) {
        emit faceClicked(id);
        return;
    }
    // سحب الصورة المكبّرة بالزر الأيسر، أو بالزر الأوسط في أي وقت
    if (!m_image.isNull() && ((event->button() == Qt::LeftButton && m_zoom > 1.0) || event->button() == Qt::MiddleButton)) {
        m_panning = true;
        m_panStart = event->position();
        m_panOrigin = m_pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    QWidget::mousePressEvent(event);
}

void ImageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_panning = false;
        setCursor(m_zoom > 1.0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        m_dragEnd = toImageFraction(event->position());
        const QRectF selected = QRectF(m_dragStart, m_dragEnd).normalized();
        // نقرة دون سحب تعني الإطار كله
        m_region = selected.width() > 0.03 && selected.height() > 0.03 ? selected : QRectF();
        setSelectingRegion(false);
        emit regionSelected(m_region);
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ImageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int id = m_selecting ? -1 : boxAt(event->position());
    if (id >= 0) {
        emit faceDoubleClicked(id);
        return;
    }
    // نقر مزدوج على الصورة: تكبير حول النقطة، ومرة أخرى للعودة إلى العرض الكامل
    if (!m_selecting && event->button() == Qt::LeftButton && !m_image.isNull() && imageRect().contains(event->position())) {
        if (m_zoom > 1.0)
            resetZoom();
        else
            setZoom(2.5, event->position());
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void ImageView::wheelEvent(QWheelEvent *event)
{
    if (m_image.isNull() || m_selecting) {
        QWidget::wheelEvent(event);
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0)
        return;
    setZoom(m_zoom * std::pow(1.2, steps), event->position());
    event->accept();
}

void ImageView::resizeEvent(QResizeEvent *event)
{
    clampPan();
    QWidget::resizeEvent(event);
}

void ImageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_pan = m_panOrigin + (event->position() - m_panStart);
        clampPan();
        update();
        return;
    }
    int nav = 0;
    for (int step : {-1, 1}) {
        if ((step < 0 ? m_canPrevious : m_canNext) && navButton(step).contains(event->position()))
            nav = step;
    }
    if (nav != m_navHover) {
        m_navHover = nav;
        update();
    }
    if (nav != 0) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    if (m_dragging) {
        m_dragEnd = toImageFraction(event->position());
        update();
        return;
    }
    if (m_selecting) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int id = boxAt(event->position());
    if (id != m_hovered) {
        m_hovered = id;
        update();
    }
    setCursor(id >= 0 ? Qt::PointingHandCursor : m_zoom > 1.0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void ImageView::leaveEvent(QEvent *event)
{
    m_hovered = -1;
    m_navHover = 0;
    update();
    QWidget::leaveEvent(event);
}
