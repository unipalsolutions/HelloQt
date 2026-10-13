#include "timelineview.h"

#include "imageio.h"
#include "theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int RulerHeight = 22;
constexpr int FilmHeight = 44;
constexpr int LaneHeight = 26;
constexpr int Gap = 6;
constexpr int ThumbnailHeight = FilmHeight * 2; // بدقة مضاعفة للشاشات عالية الكثافة
constexpr int MaxThumbnails = 600;

} // namespace

TimelineView::TimelineView(QWidget *parent)
    : QWidget(parent)
{
    setLayoutDirection(Qt::LeftToRight); // الزمن يتقدم من اليسار إلى اليمين كما في برامج المونتاج
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setToolTip(QStringLiteral("انقر للانتقال إلى لحظة · نقر مزدوج على مقطع شخص لعرض لقطاته\n"
                              "Ctrl+العجلة للتكبير · Shift+العجلة أو السحب بالزر الأيمن للتنقل · ←/→ إطار"));
}

void TimelineView::clear()
{
    m_duration = 0;
    m_rangeStart = m_rangeEnd = 0;
    m_analyzedUntil = -1;
    m_position = -1;
    m_thumbnails.clear();
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
    m_analyzedUntil = ms;
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

void TimelineView::zoomIn()
{
    const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : double(m_duration) / trackWidth();
    setZoom(scale / 2.0, m_position >= 0 ? xOf(m_position) : trackLeft() + trackWidth() / 2);
}

void TimelineView::zoomOut()
{
    const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : double(m_duration) / trackWidth();
    setZoom(scale * 2.0, m_position >= 0 ? xOf(m_position) : trackLeft() + trackWidth() / 2);
}

void TimelineView::fit()
{
    m_msPerPixel = 0.0;
    m_viewStart = 0.0;
    update();
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
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));

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
    p.setFont(small);
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
    if (m_analyzedUntil >= 0) {
        const double x = std::max(left, xOf(m_analyzedUntil));
        p.fillRect(QRectF(x, RulerHeight, right - x, FilmHeight), QColor(0, 0, 0, 90));
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
            p.drawPixmap(QRectF(left - 4 - 6 - 20, y + 3, 20, 20), lane.avatar, lane.avatar.rect());
        p.setPen(selected ? Theme::Text : Theme::Muted);
        p.setFont(small);
        const QRectF textRect(8, y, left - 4 - 6 - 20 - 14, LaneHeight);
        p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(lane.name, Qt::ElideRight, int(textRect.width())));
    }
    p.restore();
    p.setPen(Theme::Muted);
    p.setFont(small);
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
    const QPointF pos = event->position();
    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) {
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
    }
    m_scrubbing = true;
    setPosition(t);
    emit seekRequested(t);
}

void TimelineView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    m_hoverX = pos.x();
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

void TimelineView::mouseReleaseEvent(QMouseEvent *)
{
    m_scrubbing = false;
    if (m_panning) {
        m_panning = false;
        unsetCursor();
    }
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
        const double scale = m_msPerPixel > 0.0 ? m_msPerPixel : double(m_duration) / trackWidth();
        setZoom(scale * std::pow(0.8, delta.y() / 120.0), std::max(x, trackLeft()));
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
    if (m_duration <= 0 || m_position < 0)
        return QWidget::keyPressEvent(event);
    qint64 step = qint64(std::llround(1000.0 / m_fps));
    if (event->modifiers() & Qt::ShiftModifier)
        step = 5000;
    qint64 target = m_position;
    switch (event->key()) {
    case Qt::Key_Right:
        target += step;
        break;
    case Qt::Key_Left:
        target -= step;
        break;
    case Qt::Key_Home:
        target = 0;
        break;
    case Qt::Key_End:
        target = m_duration - 1;
        break;
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
    target = std::clamp<qint64>(target, 0, m_duration - 1);
    setPosition(target);
    emit seekRequested(target);
}

void TimelineView::leaveEvent(QEvent *event)
{
    m_hoverX = -1.0;
    update();
    QWidget::leaveEvent(event);
}
