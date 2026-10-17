#include "timelineview.h"

#include "imageio.h"
#include "theme.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int RulerHeight = 22;
constexpr int FilmHeight = 44;
constexpr int LaneHeight = 28;
constexpr int Gap = 6;
constexpr int ThumbnailHeight = FilmHeight * 2; // بدقة مضاعفة للشاشات عالية الكثافة
constexpr int MaxThumbnails = 600;
constexpr int MaxPreviews = 400;
constexpr int PreviewWidth = 200;
constexpr qint64 PreviewToleranceMs = 700; // مصغرة أبعد من هذا عن موضع الفأرة تُطلب من جديد

} // namespace

// معاينة الإطار فوق الخط الزمني عند المرور بالفأرة، كنافذة تلميح لا تأخذ التركيز ولا النقر
class TimelinePreview : public QWidget
{
public:
    explicit TimelinePreview(QWidget *owner)
        : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
    }

    void setContent(const QImage &image, const QString &time, const QString &note)
    {
        m_image = image;
        m_time = time;
        m_note = note;
        const int imageHeight = image.isNull() ? 0 : PreviewWidth * image.height() / std::max(1, image.width());
        const int noteHeight = note.isEmpty() ? 0 : 20;
        resize(PreviewWidth + 8, imageHeight + 26 + noteHeight + (image.isNull() ? 0 : 4));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        p.fillPath(frame, Theme::Surface);
        p.setPen(QPen(Theme::Border, 1));
        p.drawPath(frame);
        int y = 4;
        if (!m_image.isNull()) {
            const int h = PreviewWidth * m_image.height() / std::max(1, m_image.width());
            QPainterPath clip;
            clip.addRoundedRect(QRectF(4, y, PreviewWidth, h), 5, 5);
            p.setClipPath(clip);
            p.drawImage(QRectF(4, y, PreviewWidth, h), m_image);
            p.setClipping(false);
            y += h + 2;
        }
        p.setPen(Theme::Text);
        p.drawText(QRectF(4, y, PreviewWidth, 22), Qt::AlignCenter, m_time);
        y += 22;
        if (!m_note.isEmpty()) {
            p.setPen(QColor(0xfa, 0xcc, 0x15));
            p.drawText(QRectF(6, y, PreviewWidth - 4, 18), Qt::AlignCenter,
                       p.fontMetrics().elidedText(m_note, Qt::ElideRight, PreviewWidth - 8));
        }
    }

private:
    QImage m_image;
    QString m_time;
    QString m_note;
};

TimelineView::TimelineView(QWidget *parent)
    : QWidget(parent)
{
    setLayoutDirection(Qt::LeftToRight); // الزمن يتقدم من اليسار إلى اليمين كما في برامج المونتاج
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_preview = new TimelinePreview(this);
    m_zoomAnimation = new QVariantAnimation(this);
    m_zoomAnimation->setDuration(180);
    m_zoomAnimation->setEasingCurve(QEasingCurve::OutCubic);
    // الحركة على لوغاريتم المقياس فيبدو التكبير متساوي السرعة
    connect(m_zoomAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { setZoom(std::exp(value.toDouble()), m_zoomAnchor); });
}

void TimelineView::clear()
{
    m_duration = 0;
    m_rangeStart = m_rangeEnd = 0;
    m_analyzedUntil = -1;
    m_position = -1;
    m_thumbnails.clear();
    m_previews.clear();
    m_markers.clear();
    m_shotChanges.clear();
    m_requestedPreview = -1;
    m_zoomAnimation->stop();
    hidePreview();
    m_lanes.clear();
    m_selectedLane = -1;
    m_msPerPixel = 0.0;
    m_viewStart = 0.0;
    m_laneScroll = 0;
    update();
}

void TimelineView::setDuration(qint64 durationMs)
{
    m_duration = std::max<qint64>(0, durationMs);
    clampView();
    update();
}

void TimelineView::setRange(qint64 startMs, qint64 endMs)
{
    m_rangeStart = startMs;
    m_rangeEnd = endMs;
    update();
}

void TimelineView::setAnalyzedUntil(qint64 ms)
{
    // التحليل قد يعود إلى جزء تخطاه ليلحق العرض: الحد لا يرجع إلى الوراء
    m_analyzedUntil = ms < 0 ? -1 : std::max(m_analyzedUntil, ms);
    update();
}

void TimelineView::setPosition(qint64 ms)
{
    if (ms == m_position)
        return;
    m_position = ms;
    if (!m_scrubbing)
        ensurePlayheadVisible();
    update();
}

bool TimelineView::wantsThumbnail(qint64 ms) const
{
    if (m_duration <= 0)
        return m_thumbnails.isEmpty();
    const qint64 spacing = std::max<qint64>(1000, m_duration / MaxThumbnails);
    auto it = m_thumbnails.lowerBound(ms - spacing + 1);
    return it == m_thumbnails.end() || it.key() >= ms + spacing;
}

void TimelineView::addThumbnail(qint64 ms, const QImage &frame)
{
    if (frame.isNull())
        return;
    m_thumbnails.insert(ms, frame.height() > ThumbnailHeight
                                ? frame.scaledToHeight(ThumbnailHeight, Qt::SmoothTransformation)
                                : frame);
    update();
}

void TimelineView::setThumbnails(const QMap<qint64, QImage> &thumbnails)
{
    m_thumbnails = thumbnails;
    update();
}

void TimelineView::setLanes(const QList<TimelineLane> &lanes)
{
    m_lanes = lanes;
    m_laneScroll = std::clamp(m_laneScroll, 0, std::max(0, int(m_lanes.size()) - visibleLaneCount()));
    update();
}

void TimelineView::setSelectedLane(int id)
{
    if (id == m_selectedLane)
        return;
    m_selectedLane = id;
    // التمرير إلى مسار الشخص المحدد
    for (int i = 0; i < m_lanes.size(); ++i) {
        if (m_lanes.at(i).id != id)
            continue;
        const int visible = visibleLaneCount();
        if (i < m_laneScroll)
            m_laneScroll = i;
        else if (visible > 0 && i >= m_laneScroll + visible)
            m_laneScroll = i - visible + 1;
        break;
    }
    update();
}

void TimelineView::setMarkers(const QMap<qint64, QString> &markers)
{
    m_markers = markers;
    update();
}

void TimelineView::setShotChanges(const QList<qint64> &changes)
{
    m_shotChanges = changes;
    update();
}

void TimelineView::setPreviewFrame(qint64 ms, const QImage &frame)
{
    if (frame.isNull())
        return;
    const QImage scaled = frame.height() > ThumbnailHeight ? frame.scaledToHeight(ThumbnailHeight, Qt::SmoothTransformation) : frame;
    if (m_previews.size() >= MaxPreviews)
        m_previews.erase(std::abs(m_previews.firstKey() - ms) > std::abs(m_previews.lastKey() - ms) ? m_previews.begin()
                                                                                                    : std::prev(m_previews.end()));
    m_previews.insert(ms, scaled);
    if (wantsThumbnail(ms))
        addThumbnail(ms, scaled);
    if (m_requestedPreview == ms)
        m_requestedPreview = -1;
    if (m_preview->isVisible() && m_hoverX >= 0)
        updatePreview(QPointF(m_hoverX, RulerHeight));
}

void TimelineView::setLabelWidth(int width)
{
    m_labelWidth = width;
    update();
}

QSize TimelineView::sizeHint() const
{
    const int lanes = std::clamp(int(m_lanes.size()), 2, 4);
    return {600, lanesTop() + lanes * LaneHeight + 6};
}

QSize TimelineView::minimumSizeHint() const
{
    return {240, RulerHeight + FilmHeight + Gap + 2 * LaneHeight + 4};
}

int TimelineView::lanesTop() const
{
    return RulerHeight + FilmHeight + Gap;
}

int TimelineView::visibleLaneCount() const
{
    return std::max(0, (height() - lanesTop() - 2) / LaneHeight);
}

double TimelineView::xOf(qint64 ms) const
{
    const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : std::max(1.0, double(m_duration)) / trackWidth();
    return trackLeft() + (ms - m_viewStart) / scale;
}

qint64 TimelineView::timeAt(double x) const
{
    const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : std::max(1.0, double(m_duration)) / trackWidth();
    return std::clamp<qint64>(qint64(m_viewStart + (x - trackLeft()) * scale), 0, std::max<qint64>(0, m_duration - 1));
}

int TimelineView::laneAt(double y) const
{
    if (y < lanesTop())
        return -1;
    const int index = m_laneScroll + int((y - lanesTop()) / LaneHeight);
    return index < m_lanes.size() ? index : -1;
}

void TimelineView::setZoom(double msPerPixel, double anchorX)
{
    if (m_duration <= 0)
        return;
    const double fitScale = double(m_duration) / trackWidth();
    const double minScale = std::max(1000.0 / m_fps / 24.0, 1.0); // أقصى تكبير: الإطار نحو 24 بكسل
    const double anchorTime = m_viewStart + (anchorX - trackLeft()) * (m_msPerPixel > 0.0 ? m_msPerPixel : fitScale);
    msPerPixel = std::clamp(msPerPixel, minScale, fitScale);
    m_msPerPixel = msPerPixel >= fitScale * 0.999 ? 0.0 : msPerPixel;
    m_viewStart = m_msPerPixel > 0.0 ? anchorTime - (anchorX - trackLeft()) * m_msPerPixel : 0.0;
    clampView();
    update();
}

void TimelineView::clampView()
{
    if (m_msPerPixel <= 0.0 || m_duration <= 0) {
        m_msPerPixel = 0.0;
        m_viewStart = 0.0;
        return;
    }
    const double visible = trackWidth() * m_msPerPixel;
    m_viewStart = std::clamp(m_viewStart, 0.0, std::max(0.0, m_duration - visible));
}

void TimelineView::ensurePlayheadVisible()
{
    if (m_msPerPixel <= 0.0 || m_position < 0)
        return;
    const double visible = trackWidth() * m_msPerPixel;
    if (m_position < m_viewStart || m_position > m_viewStart + visible) {
        m_viewStart = m_position - visible * 0.25;
        clampView();
    }
}

double TimelineView::currentScale() const
{
    return m_msPerPixel > 0.0 ? m_msPerPixel : std::max(1.0, double(m_duration)) / trackWidth();
}

void TimelineView::animateZoom(double msPerPixel, double anchorX)
{
    if (m_duration <= 0)
        return;
    const double fitScale = double(m_duration) / trackWidth();
    const double minScale = std::max(1000.0 / m_fps / 24.0, 1.0);
    m_zoomTarget = std::clamp(msPerPixel, minScale, fitScale);
    m_zoomAnchor = anchorX;
    m_zoomAnimation->stop();
    m_zoomAnimation->setStartValue(std::log(currentScale()));
    m_zoomAnimation->setEndValue(std::log(m_zoomTarget));
    m_zoomAnimation->start();
}

void TimelineView::zoomIn()
{
    const double scale = m_zoomAnimation->state() == QAbstractAnimation::Running ? m_zoomTarget : currentScale();
    animateZoom(scale / 2.0, m_position >= 0 && xOf(m_position) >= trackLeft() ? xOf(m_position) : trackLeft() + trackWidth() / 2);
}

void TimelineView::zoomOut()
{
    const double scale = m_zoomAnimation->state() == QAbstractAnimation::Running ? m_zoomTarget : currentScale();
    animateZoom(scale * 2.0, m_position >= 0 && xOf(m_position) >= trackLeft() ? xOf(m_position) : trackLeft() + trackWidth() / 2);
}

void TimelineView::fit()
{
    animateZoom(double(m_duration) / trackWidth(), trackLeft());
}

void TimelineView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampView();
    m_laneScroll = std::clamp(m_laneScroll, 0, std::max(0, int(m_lanes.size()) - visibleLaneCount()));
}

void TimelineView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), Theme::Surface);

    const double left = trackLeft();
    const double right = left + trackWidth();
    const QRectF track(left, 0, right - left, height());
    QFont caption = font();
    caption.setPointSizeF(std::max(7.0, caption.pointSizeF() - 1.5));

    // عمود الأسماء
    p.fillRect(QRectF(0, 0, left - 4, height()), Theme::mix(Theme::Surface, Theme::Background, 0.5));

    if (m_duration <= 0) {
        p.setPen(Theme::Muted);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("سيظهر هنا الخط الزمني للفيديو ومسار كل شخص فيه"));
        return;
    }

    p.save();
    p.setClipRect(track);

    // ── المسطرة ──
    const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : double(m_duration) / trackWidth();
    static const qint64 steps[] = {40, 100, 200, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000,
                                   600000, 900000, 1800000, 3600000, 7200000};
    qint64 step = steps[std::size(steps) - 1];
    for (qint64 s : steps) {
        if (s / scale >= 90.0) {
            step = s;
            break;
        }
    }
    p.setFont(caption);
    const qint64 first = qint64(std::floor(m_viewStart / step)) * step;
    for (qint64 t = first; xOf(t) <= right; t += step) {
        const double x = xOf(t);
        p.setPen(QPen(Theme::Border, 1));
        p.drawLine(QPointF(x, RulerHeight - 7), QPointF(x, RulerHeight));
        for (int k = 1; k < 5; ++k) {
            const double xs = xOf(t + step * k / 5);
            p.drawLine(QPointF(xs, RulerHeight - 3), QPointF(xs, RulerHeight));
        }
        p.setPen(Theme::Muted);
        QString text = ImageIO::formatTime(t);
        if (step < 1000)
            text += QStringLiteral(".%1").arg((t % 1000) / 100);
        p.drawText(QRectF(x + 3, 2, 90, RulerHeight - 8), Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    // ── شريط المصغرات ──
    const QRectF film(left, RulerHeight, right - left, FilmHeight);
    p.fillRect(film, Theme::Background);
    if (!m_thumbnails.isEmpty()) {
        const QImage &sample = m_thumbnails.first();
        const double tileWidth = std::max(30.0, FilmHeight * double(sample.width()) / std::max(1, sample.height()));
        for (double x = left; x < right; x += tileWidth) {
            const qint64 t = timeAt(x + tileWidth / 2);
            auto it = m_thumbnails.lowerBound(t);
            if (it == m_thumbnails.end() || (it != m_thumbnails.begin() && t - std::prev(it).key() < it.key() - t))
                it = std::prev(it);
            // لا تُكرر مصغرة بعيدة جداً عن هذا الموضع
            if (std::llabs(it.key() - t) > std::max<qint64>(4000, qint64(tileWidth * scale * 2)))
                continue;
            p.drawImage(QRectF(x, RulerHeight + 1, tileWidth - 1, FilmHeight - 2), it.value());
        }
    }
    // الأجزاء غير المطلوبة أو التي لم تُحلل بعد
    const QColor dim(0, 0, 0, 150);
    if (m_rangeStart > 0)
        p.fillRect(QRectF(left, RulerHeight, xOf(m_rangeStart) - left, height() - RulerHeight), dim);
    if (m_rangeEnd > 0)
        p.fillRect(QRectF(xOf(m_rangeEnd), RulerHeight, right - xOf(m_rangeEnd), height() - RulerHeight), dim);
    // مقبضا الاقتطاع
    if (m_duration > 0) {
        const QColor trim(0xfa, 0xcc, 0x15);
        const qint64 endMs = m_rangeEnd > 0 ? m_rangeEnd : m_duration;
        const double sx = xOf(m_rangeStart), ex = xOf(endMs);
        if (m_rangeStart > 0 || m_rangeEnd > 0) {
            p.setPen(QPen(trim, 2));
            p.drawLine(QPointF(sx, RulerHeight + 1), QPointF(ex, RulerHeight + 1));
            p.drawLine(QPointF(sx, RulerHeight + FilmHeight - 1), QPointF(ex, RulerHeight + FilmHeight - 1));
        }
        for (const auto &[x, isStart] : {std::pair<double, bool>{sx, true}, std::pair<double, bool>{ex, false}}) {
            if (x < left - 8 || x > right + 8)
                continue;
            const double w = 7.0;
            const QRectF grip(isStart ? x : x - w, RulerHeight, w, FilmHeight);
            QPainterPath path;
            path.addRoundedRect(grip, 2, 2);
            p.fillPath(path, (m_dragHandle == (isStart ? 1 : 2)) ? trim.lighter(120) : trim);
            p.setPen(QPen(QColor(0, 0, 0, 140), 1));
            const double cx = grip.center().x();
            p.drawLine(QPointF(cx, grip.top() + 14), QPointF(cx, grip.bottom() - 14));
        }
    }
    if (m_analyzedUntil >= 0) {
        const double x = std::max(left, xOf(m_analyzedUntil));
        p.fillRect(QRectF(x, RulerHeight, right - x, FilmHeight), QColor(0, 0, 0, 90));
    }
    // تغيّرات اللقطات: خط أبيض قصير أسفل شريط المصغرات
    p.setPen(QPen(QColor(255, 255, 255, 170), 1.5));
    for (qint64 t : std::as_const(m_shotChanges)) {
        const double x = xOf(t);
        if (x >= left && x <= right)
            p.drawLine(QPointF(x, RulerHeight + FilmHeight - 9), QPointF(x, RulerHeight + FilmHeight));
    }
    // العلامات: راية صفراء على المسطرة وخط خفيف عبر الشريط
    const QColor flag(0xfa, 0xcc, 0x15);
    for (auto it = m_markers.cbegin(); it != m_markers.cend(); ++it) {
        const double x = xOf(it.key());
        if (x < left - 6 || x > right + 6)
            continue;
        p.setPen(QPen(QColor(flag.red(), flag.green(), flag.blue(), 110), 1));
        p.drawLine(QPointF(x, RulerHeight), QPointF(x, RulerHeight + FilmHeight));
        QPainterPath banner;
        banner.moveTo(x, 3);
        banner.lineTo(x + 8, 3);
        banner.lineTo(x + 6, 7);
        banner.lineTo(x + 8, 11);
        banner.lineTo(x, 11);
        banner.closeSubpath();
        p.fillPath(banner, flag);
        p.setPen(QPen(flag, 1.5));
        p.drawLine(QPointF(x, 3), QPointF(x, RulerHeight));
    }

    // ── مسارات الأشخاص ──
    const int top = lanesTop();
    const int visible = visibleLaneCount() + 1;
    for (int i = m_laneScroll; i < m_lanes.size() && i < m_laneScroll + visible; ++i) {
        const TimelineLane &lane = m_lanes.at(i);
        const double y = top + (i - m_laneScroll) * LaneHeight;
        const bool selected = lane.id == m_selectedLane;
        if (selected)
            p.fillRect(QRectF(left, y, right - left, LaneHeight), Theme::mix(Theme::Surface, Theme::Accent, 0.12));
        else if ((i % 2) == 1)
            p.fillRect(QRectF(left, y, right - left, LaneHeight), Theme::mix(Theme::Surface, Theme::Background, 0.35));
        QColor fill = lane.color;
        fill.setAlpha(selected ? 235 : 170);
        for (const auto &segment : lane.segments) {
            const double x1 = xOf(segment.first);
            const double x2 = std::max(x1 + 3.0, xOf(segment.second));
            if (x2 < left || x1 > right)
                continue;
            QPainterPath path;
            path.addRoundedRect(QRectF(x1, y + 5, x2 - x1, LaneHeight - 10), 4, 4);
            p.fillPath(path, fill);
            if (selected) {
                p.setPen(QPen(lane.color.lighter(140), 1));
                p.drawPath(path);
            }
        }
    }
    p.restore();

    // أسماء المسارات وصورها (اتجاه النص العربي داخل العمود)
    p.save();
    p.setClipRect(QRectF(0, top, left - 4, height() - top));
    for (int i = m_laneScroll; i < m_lanes.size() && i < m_laneScroll + visible; ++i) {
        const TimelineLane &lane = m_lanes.at(i);
        const double y = top + (i - m_laneScroll) * LaneHeight;
        const bool selected = lane.id == m_selectedLane;
        if (selected)
            p.fillRect(QRectF(0, y, left - 4, LaneHeight), Theme::mix(Theme::Surface, Theme::Accent, 0.22));
        p.fillRect(QRectF(0, y + 4, 3, LaneHeight - 8), lane.color);
        if (!lane.avatar.isNull())
            p.drawPixmap(QRectF(left - 4 - 6 - 22, y + 3, 22, 22), lane.avatar, lane.avatar.rect());
        if (lane.watched) {
            p.setPen(QPen(Theme::Accent2, 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(left - 4 - 6 - 23, y + 2, 24, 24));
        }
        p.setPen(selected ? Theme::Text : Theme::Muted);
        p.setFont(caption);
        const QRectF textRect(8, y, left - 4 - 6 - 22 - 14, LaneHeight);
        p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(lane.name, Qt::ElideRight, int(textRect.width())));
    }
    p.restore();
    p.setPen(Theme::Muted);
    p.setFont(caption);
    p.drawText(QRectF(6, RulerHeight, left - 16, FilmHeight), Qt::AlignRight | Qt::AlignVCenter,
               m_lanes.isEmpty() ? QStringLiteral("الإطارات") : QStringLiteral("الأشخاص: %1").arg(m_lanes.size()));
    // مؤشر وجود مسارات أخرى بالتمرير
    if (m_lanes.size() > visibleLaneCount()) {
        const double barTop = top;
        const double barHeight = height() - top - 2;
        const double thumb = std::max(12.0, barHeight * visibleLaneCount() / m_lanes.size());
        const double offset = (barHeight - thumb) * m_laneScroll / std::max(1, int(m_lanes.size()) - visibleLaneCount());
        p.fillRect(QRectF(width() - 5, barTop + offset, 3, thumb), Theme::Border);
    }

    // ── مؤشر التمرير ومؤشر الموضع ──
    if (m_hoverX >= left && m_hoverX <= right) {
        p.setPen(QPen(QColor(255, 255, 255, 60), 1));
        p.drawLine(QPointF(m_hoverX, RulerHeight), QPointF(m_hoverX, height()));
    }
    if (m_position >= 0) {
        const double x = xOf(m_position);
        if (x >= left - 1 && x <= right + 1) {
            p.setPen(QPen(Theme::Accent2, 2));
            p.drawLine(QPointF(x, 6), QPointF(x, height()));
            QPainterPath head;
            head.moveTo(x - 6, 2);
            head.lineTo(x + 6, 2);
            head.lineTo(x + 6, 10);
            head.lineTo(x, 16);
            head.lineTo(x - 6, 10);
            head.closeSubpath();
            p.fillPath(head, Theme::Accent2);
        }
    }
    // حد المنطقة
    p.setPen(QPen(Theme::Border, 1));
    p.drawLine(QPointF(left - 4, 0), QPointF(left - 4, height()));
    p.drawLine(QPointF(0, RulerHeight + 0.5), QPointF(width(), RulerHeight + 0.5));
}

void TimelineView::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    hidePreview();
    const QPointF pos = event->position();
    if (event->button() == Qt::RightButton) {
        // نقرة يمنى = قائمة العمليات، وسحب بالزر الأيمن = تنقل
        m_rightPressed = true;
        m_panOrigin = pos;
        m_panStart = m_viewStart;
        return;
    }
    if (event->button() == Qt::LeftButton && (m_dragHandle = handleAt(pos)) != 0) {
        setCursor(Qt::SizeHorCursor);
        update();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panOrigin = pos;
        m_panStart = m_viewStart;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton || m_duration <= 0)
        return;
    const int lane = laneAt(pos.y());
    if (pos.x() < trackLeft()) {
        if (lane >= 0)
            emit laneClicked(m_lanes.at(lane).id, -1);
        return;
    }
    const qint64 t = timeAt(pos.x());
    if (lane >= 0) {
        // النقر على مقطع شخص يحدده؛ وإلا فهو انتقال عادي
        for (const auto &segment : m_lanes.at(lane).segments) {
            if (xOf(segment.first) - 3 <= pos.x() && pos.x() <= std::max(xOf(segment.first) + 3, xOf(segment.second)) + 3) {
                const qint64 inside = std::clamp(t, segment.first, segment.second);
                emit laneClicked(m_lanes.at(lane).id, inside);
                setPosition(inside);
                emit seekRequested(inside);
                return;
            }
        }
        // النقر على مسار الشخص خارج مقاطعه يقفز إلى أقرب ظهور له
        const QList<QPair<qint64, qint64>> &segments = m_lanes.at(lane).segments;
        if (!segments.isEmpty()) {
            auto distance = [t](const QPair<qint64, qint64> &s) { return t < s.first ? s.first - t : std::max<qint64>(0, t - s.second); };
            const auto nearest = std::min_element(segments.cbegin(), segments.cend(),
                                                  [&](const auto &a, const auto &b) { return distance(a) < distance(b); });
            emit laneClicked(m_lanes.at(lane).id, nearest->first);
            setPosition(nearest->first);
            emit seekRequested(nearest->first);
            return;
        }
    }
    m_scrubbing = true;
    setPosition(t);
    emit seekRequested(t);
}

void TimelineView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    m_hoverX = pos.x();
    if (m_rightPressed && !m_panning && std::abs(pos.x() - m_panOrigin.x()) > 4) {
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (m_dragHandle != 0) {
        const qint64 t = std::clamp<qint64>(timeAt(pos.x()), 0, m_duration);
        const qint64 endMs = m_rangeEnd > 0 ? m_rangeEnd : m_duration;
        const qint64 minimum = 500;
        if (m_dragHandle == 1)
            m_rangeStart = std::clamp<qint64>(t, 0, std::max<qint64>(0, endMs - minimum));
        else
            m_rangeEnd = std::clamp<qint64>(t, m_rangeStart + minimum, m_duration);
        setPosition(m_dragHandle == 1 ? m_rangeStart : m_rangeEnd);
        emit seekRequested(m_position);
    } else if (event->buttons() == Qt::NoButton) {
        if (handleAt(pos) != 0)
            setCursor(Qt::SizeHorCursor);
        else
            unsetCursor();
        const int lane = laneAt(pos.y());
        if (pos.x() < trackLeft()) {
            hidePreview();
            if (lane >= 0 && !m_lanes.at(lane).detail.isEmpty())
                QToolTip::showText(event->globalPosition().toPoint(), m_lanes.at(lane).detail, this);
        } else {
            updatePreview(pos);
        }
    }
    if (m_panning) {
        const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : double(m_duration) / trackWidth();
        m_viewStart = m_panStart - (pos.x() - m_panOrigin.x()) * scale;
        clampView();
    } else if (m_scrubbing) {
        const qint64 t = timeAt(pos.x());
        if (t != m_position) {
            setPosition(t);
            emit seekRequested(t);
        }
    }
    update();
}

void TimelineView::updatePreview(const QPointF &pos)
{
    if (m_duration <= 0 || pos.x() < trackLeft() || pos.x() > trackLeft() + trackWidth()) {
        hidePreview();
        return;
    }
    const qint64 t = timeAt(pos.x());
    m_hoverTime = t;
    auto nearest = [t](const QMap<qint64, QImage> &images) -> QPair<qint64, QImage> {
        if (images.isEmpty())
            return {-1, {}};
        auto it = images.lowerBound(t);
        if (it == images.end() || (it != images.begin() && t - std::prev(it).key() < it.key() - t))
            it = std::prev(it);
        return {it.key(), it.value()};
    };
    QPair<qint64, QImage> best = nearest(m_previews);
    const QPair<qint64, QImage> thumb = nearest(m_thumbnails);
    if (best.first < 0 || (thumb.first >= 0 && std::llabs(thumb.first - t) < std::llabs(best.first - t)))
        best = thumb;
    const bool close = best.first >= 0 && std::llabs(best.first - t) <= PreviewToleranceMs;
    if (!close && (m_requestedPreview < 0 || std::llabs(m_requestedPreview - t) > PreviewToleranceMs / 2)) {
        m_requestedPreview = t;
        emit previewRequested(t);
    }
    // علامة قريبة من الفأرة تظهر ملاحظتها في المعاينة
    QString note;
    const double scale = currentScale();
    for (auto it = m_markers.lowerBound(qint64(t - 8 * scale)); it != m_markers.cend() && it.key() <= t + 8 * scale; ++it) {
        note = it.value().isEmpty() ? QStringLiteral("علامة") : it.value();
        break;
    }
    QString time = ImageIO::formatTime(t);
    if (scale < 200.0)
        time += QStringLiteral(".%1").arg(t % 1000 / 100);
    // صورة بعيدة (أثناء انتظار المطلوبة) أفضل من لا شيء إن كانت في حدود ثوانٍ
    const bool usable = best.first >= 0 && std::llabs(best.first - t) <= std::max<qint64>(PreviewToleranceMs * 4, qint64(scale * 20));
    m_preview->setContent(usable ? best.second : QImage(), time, note);
    const QPoint anchor = mapToGlobal(QPoint(int(pos.x()), 0));
    QPoint topLeft(anchor.x() - m_preview->width() / 2, anchor.y() - m_preview->height() - 6);
    if (const QScreen *screen = QGuiApplication::screenAt(anchor)) {
        const QRect area = screen->availableGeometry();
        topLeft.setX(std::clamp(topLeft.x(), area.left(), std::max(area.left(), area.right() - m_preview->width())));
        if (topLeft.y() < area.top())
            topLeft.setY(mapToGlobal(QPoint(0, RulerHeight + FilmHeight + 6)).y());
    }
    m_preview->move(topLeft);
    if (!m_preview->isVisible())
        m_preview->show();
}

void TimelineView::hidePreview()
{
    m_hoverTime = -1;
    if (m_preview && m_preview->isVisible())
        m_preview->hide();
}

void TimelineView::mouseReleaseEvent(QMouseEvent *event)
{
    m_scrubbing = false;
    if (m_dragHandle != 0) {
        m_dragHandle = 0;
        unsetCursor();
        editRange(m_rangeStart, m_rangeEnd >= m_duration ? 0 : m_rangeEnd);
        return;
    }
    if (event->button() == Qt::RightButton && m_rightPressed) {
        m_rightPressed = false;
        if (!m_panning && m_duration > 0) {
            const QPointF pos = event->position();
            const int lane = laneAt(pos.y());
            const qint64 t = pos.x() >= trackLeft() ? std::clamp<qint64>(timeAt(pos.x()), 0, m_duration - 1) : m_position;
            emit contextMenuRequested(event->globalPosition().toPoint(), t, lane >= 0 ? m_lanes.at(lane).id : -1);
        }
    }
    if (m_panning) {
        m_panning = false;
        unsetCursor();
    }
}

int TimelineView::handleAt(const QPointF &pos) const
{
    if (m_duration <= 0 || pos.y() < RulerHeight || pos.y() > RulerHeight + FilmHeight)
        return 0;
    const double sx = xOf(m_rangeStart);
    const double ex = xOf(m_rangeEnd > 0 ? m_rangeEnd : m_duration);
    const double ds = std::abs(pos.x() - sx), de = std::abs(pos.x() - ex);
    if (std::min(ds, de) > 8.0)
        return 0;
    return ds <= de ? 1 : 2;
}

void TimelineView::editRange(qint64 startMs, qint64 endMs)
{
    m_rangeStart = std::max<qint64>(0, startMs);
    m_rangeEnd = endMs > 0 && endMs < m_duration ? endMs : 0;
    update();
    emit rangeEdited(m_rangeStart, m_rangeEnd);
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (pos.y() < RulerHeight) {
        fit();
        return;
    }
    const int lane = laneAt(pos.y());
    if (lane >= 0)
        emit laneActivated(m_lanes.at(lane).id, pos.x() >= trackLeft() ? timeAt(pos.x()) : -1);
}

void TimelineView::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    const double x = event->position().x();
    if (event->modifiers() & Qt::ControlModifier) {
        const double scale = m_zoomAnimation->state() == QAbstractAnimation::Running ? m_zoomTarget : currentScale();
        animateZoom(scale * std::pow(0.8, delta.y() / 120.0), std::max(x, trackLeft()));
    } else if ((event->modifiers() & Qt::ShiftModifier) || delta.x() != 0
               || event->position().y() < lanesTop() || m_lanes.size() <= visibleLaneCount()) {
        const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : 0.0;
        const int amount = delta.x() != 0 ? delta.x() : delta.y();
        m_viewStart -= amount / 120.0 * 80.0 * scale;
        clampView();
    } else {
        m_laneScroll = std::clamp(m_laneScroll - delta.y() / 120, 0, std::max(0, int(m_lanes.size()) - visibleLaneCount()));
    }
    update();
    event->accept();
}

void TimelineView::keyPressEvent(QKeyEvent *event)
{
    // الأسهم وHome وEnd وI وO تمر إلى النافذة، فتعمل كما تعمل من أي مكان آخر
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        return;
    case Qt::Key_Minus:
        zoomOut();
        return;
    default:
        return QWidget::keyPressEvent(event);
    }
}

void TimelineView::hideEvent(QHideEvent *event)
{
    hidePreview();
    QWidget::hideEvent(event);
}

void TimelineView::leaveEvent(QEvent *event)
{
    hidePreview();
    m_hoverX = -1.0;
    update();
    QWidget::leaveEvent(event);
}
