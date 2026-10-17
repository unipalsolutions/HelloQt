#include "carddelegates.h"

#include "facetypes.h"
#include "icons.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QPainterPath>

#include <algorithm>

namespace {

struct CardLayout
{
    QRectF card;
    QRectF avatar;
    QRectF text; // المساحة المتبقية للنصوص
    bool rtl;
};

CardLayout layoutCard(const QStyleOptionViewItem &option, double avatarSize)
{
    CardLayout l;
    l.rtl = option.direction == Qt::RightToLeft;
    l.card = QRectF(option.rect).adjusted(4, 4, -4, -4);
    const double y = l.card.center().y() - avatarSize / 2.0;
    l.avatar = l.rtl ? QRectF(l.card.right() - 14 - avatarSize, y, avatarSize, avatarSize)
                     : QRectF(l.card.left() + 14, y, avatarSize, avatarSize);
    l.text = l.rtl ? QRectF(l.card.left() + 14, l.card.top() + 10, l.avatar.left() - l.card.left() - 28, l.card.height() - 20)
                   : QRectF(l.avatar.right() + 14, l.card.top() + 10, l.card.right() - l.avatar.right() - 28, l.card.height() - 20);
    return l;
}

void paintCardBackground(QPainter *p, const QStyleOptionViewItem &option, const QRectF &card)
{
    using namespace Theme;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    QColor fill = selected ? mix(Surface, Accent, 0.16) : hovered ? SurfaceHigh : withAlpha(SurfaceHigh, 110);
    p->setPen(QPen(selected ? withAlpha(Accent, 200) : withAlpha(Border, hovered ? 255 : 140), selected ? 1.5 : 1.0));
    p->setBrush(fill);
    p->drawRoundedRect(card, 14, 14);
}

void paintAvatar(QPainter *p, const QRectF &rect, const QPixmap &pixmap, const QColor &ring)
{
    using namespace Theme;
    if (!pixmap.isNull()) {
        p->drawPixmap(rect.toRect(), pixmap);
    } else {
        p->setPen(Qt::NoPen);
        p->setBrush(SurfaceHigh);
        p->drawEllipse(rect);
        Icons::paint(*p, Icons::Name::Users, rect.adjusted(rect.width() * 0.25, rect.height() * 0.25,
                                                           -rect.width() * 0.25, -rect.height() * 0.25), Muted);
    }
    p->setBrush(Qt::NoBrush);
    p->setPen(QPen(ring, 2.5));
    p->drawEllipse(rect.adjusted(-1.5, -1.5, 1.5, 1.5));
}

Qt::Alignment startAlign(bool rtl)
{
    return (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignAbsolute;
}

} // namespace

QPixmap circularPixmap(const QImage &image, int size)
{
    const qreal dpr = 2.0;
    QPixmap result(int(size * dpr), int(size * dpr));
    result.fill(Qt::transparent);
    if (image.isNull())
        return QPixmap();
    const QImage scaled = image.scaled(result.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, result.width(), result.height()));
    p.setClipPath(clip);
    p.drawImage(QPointF((result.width() - scaled.width()) / 2.0, (result.height() - scaled.height()) / 2.0), scaled);
    p.end();
    result.setDevicePixelRatio(dpr);
    return result;
}

FaceCardDelegate::FaceCardDelegate(QAbstractItemView *view)
    : QStyledItemDelegate(view)
    , m_view(view)
{
    // إبراز الزر السريع تحت الفأرة يحتاج إعادة رسم البطاقة مع كل حركة، لا عند تغيّر البطاقة فقط
    if (m_view)
        m_view->viewport()->installEventFilter(this);
}

bool FaceCardDelegate::eventFilter(QObject *watched, QEvent *event)
{
    if (m_view && watched == m_view->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
            const QModelIndex before = m_view->indexAt(m_mouse);
            m_mouse = pos;
            const QModelIndex now = m_view->indexAt(pos);
            if (now.isValid() && now.data(CardRole::Actions).toInt() != 0)
                m_view->viewport()->update(m_view->visualRect(now));
            if (before.isValid() && before != now)
                m_view->viewport()->update(m_view->visualRect(before));
        } else if (event->type() == QEvent::Leave) {
            m_mouse = QPoint(-1, -1);
        }
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

QString FaceCardDelegate::actionText(int action, const QModelIndex &index)
{
    const bool watched = index.data(CardRole::Watched).toBool();
    switch (action) {
    case FaceAction::Confirm:
        return QStringLiteral("تأكيد: هذا هو %1، ويُضاف الوجه إلى صوره").arg(index.data(CardRole::Title).toString().remove(QStringLiteral("؟")));
    case FaceAction::Reject:
        return QStringLiteral("رفض التطابق: ليس هذا الشخص");
    case FaceAction::Add:
        return QStringLiteral("إضافة للقاعدة…");
    case FaceAction::Shots:
        return QStringLiteral("اللقطات");
    case FaceAction::First:
        return QStringLiteral("الانتقال إلى أول ظهور");
    case FaceAction::Watch:
        return watched ? QStringLiteral("إلغاء المراقبة") : QStringLiteral("إضافة للمراقبة: تنبيه فور ظهوره");
    default:
        break;
    }
    return {};
}

// الأزرار السريعة بترتيبها من بداية السطر؛ لا تظهر إلا عند المرور على البطاقة
QList<QPair<int, QRectF>> FaceCardDelegate::actionRects(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QList<QPair<int, QRectF>> rects;
    const int actions = index.data(CardRole::Actions).toInt();
    if (actions == 0)
        return rects;
    const bool rtl = option.direction == Qt::RightToLeft;
    const QRectF card = QRectF(option.rect).adjusted(4, 4, -4, -4);
    const double size = m_grid ? std::clamp(card.width() / 6.4, 18.0, 26.0) : 26.0;
    const double gap = m_grid ? 2.0 : 4.0;
    int count = 0;
    for (int bit = 1; bit <= FaceAction::Watch; bit <<= 1)
        count += (actions & bit) ? 1 : 0;
    const double total = count * size + (count - 1) * gap;
    double x;
    double y;
    if (m_grid) {
        // صف في أسفل الصورة
        x = card.center().x() - total / 2.0;
        y = card.top() + 8 + 72 * m_scale - size + 4;
    } else {
        // صف مكان شريط التشابه في آخر البطاقة
        const double avatar = 54 * m_scale;
        x = rtl ? card.left() + 14 : card.right() - 14 - total;
        if (!rtl)
            x = std::max(x, card.left() + 28 + avatar);
        y = card.bottom() - 10 - size;
    }
    // الترتيب البصري يتبع اتجاه الواجهة
    QList<int> order;
    for (int bit = 1; bit <= FaceAction::Watch; bit <<= 1)
        if (actions & bit)
            order << bit;
    if (rtl)
        std::reverse(order.begin(), order.end());
    for (int action : std::as_const(order)) {
        rects.append({action, QRectF(x, y, size, size)});
        x += size + gap;
    }
    return rects;
}

void FaceCardDelegate::paintActions(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    using namespace Theme;
    if (!(option.state & QStyle::State_MouseOver))
        return;
    for (const auto &[action, rect] : actionRects(option, index)) {
        const bool hot = rect.contains(m_mouse);
        const QColor tint = action == FaceAction::Confirm ? matchColor(MatchStatus::Confirmed)
                          : action == FaceAction::Reject  ? QColor(0xf8, 0x71, 0x71)
                          : action == FaceAction::Watch   ? Accent2
                                                          : Text;
        p->setPen(QPen(hot ? withAlpha(tint, 220) : withAlpha(Border, 255), 1.0));
        p->setBrush(hot ? mix(Surface, tint, 0.28) : withAlpha(Surface, 235));
        p->drawEllipse(rect);
        const double inset = rect.width() * 0.24;
        const Icons::Name icon = action == FaceAction::Confirm ? Icons::Name::Check
                               : action == FaceAction::Reject  ? Icons::Name::Reject
                               : action == FaceAction::Add     ? Icons::Name::UserPlus
                               : action == FaceAction::Shots   ? Icons::Name::Film
                               : action == FaceAction::First   ? Icons::Name::Play
                                                               : Icons::Name::Alert;
        Icons::paint(*p, icon, rect.adjusted(inset, inset, -inset, -inset), hot ? tint.lighter(120) : tint);
    }
}

// أيقونة الحالة على حافة الصورة: مؤكد، محتمل، غير معروف، أو مراقَب
static void paintStatusBadge(QPainter *p, const QRectF &avatar, MatchStatus status, bool watched, bool rtl)
{
    using namespace Theme;
    const double size = std::clamp(avatar.width() * 0.36, 16.0, 24.0);
    const QRectF badge(rtl ? avatar.left() - 2 : avatar.right() - size + 2, avatar.bottom() - size + 2, size, size);
    const QColor color = watched ? QColor(0xf8, 0x71, 0x71) : matchColor(status);
    p->setPen(QPen(Surface, 2.0));
    p->setBrush(watched ? color : mix(Surface, color, 0.85));
    p->drawEllipse(badge);
    const double inset = size * 0.24;
    const QRectF inner = badge.adjusted(inset, inset, -inset, -inset);
    if (watched) {
        Icons::paint(*p, Icons::Name::Alert, inner, Qt::white);
    } else if (status == MatchStatus::Confirmed) {
        Icons::paint(*p, Icons::Name::Check, inner, Surface);
    } else {
        QFont font = p->font();
        font.setPixelSize(int(size * 0.7));
        font.setWeight(QFont::Bold);
        p->setFont(font);
        p->setPen(Surface);
        p->drawText(badge, Qt::AlignCenter, status == MatchStatus::Possible ? QStringLiteral("؟") : QStringLiteral("—"));
    }
}

void FaceCardDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (m_grid)
        paintGrid(p, option, index);
    else
        paintList(p, option, index);
    paintActions(p, option, index);
    p->restore();
}

void FaceCardDelegate::paintList(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    using namespace Theme;
    const CardLayout l = layoutCard(option, 54 * m_scale);
    paintCardBackground(p, option, l.card);

    const auto status = MatchStatus(index.data(CardRole::Status).toInt());
    const bool watched = index.data(CardRole::Watched).toBool();
    const QColor color = matchColor(status);
    paintAvatar(p, l.avatar, index.data(CardRole::Avatar).value<QPixmap>(), watched ? QColor(0xf8, 0x71, 0x71) : color);
    const bool actions = index.data(CardRole::Actions).isValid();
    if (actions)
        paintStatusBadge(p, l.avatar, status, watched, l.rtl);

    // الاسم والحالة
    QFont titleFont = option.font;
    titleFont.setPointSizeF(11);
    titleFont.setWeight(QFont::DemiBold);
    QFont smallFont = option.font;
    smallFont.setPointSizeF(8.5);

    const QString customStatus = index.data(CardRole::StatusText).toString();
    const QString statusText = !customStatus.isEmpty()              ? customStatus
                             : watched                          ? QStringLiteral("مراقَب")
                             : status == MatchStatus::Confirmed ? QStringLiteral("مؤكد")
                             : status == MatchStatus::Possible  ? QStringLiteral("محتمل")
                                                                : QStringLiteral("غير مسجل");
    const QColor pillColor = watched && customStatus.isEmpty() ? QColor(0xf8, 0x71, 0x71) : color;
    const QFontMetricsF smallMetrics(smallFont);
    const double pillWidth = smallMetrics.horizontalAdvance(statusText) + 16;
    const QRectF pill(l.rtl ? l.text.left() : l.text.right() - pillWidth, l.text.top() + 1, pillWidth, 20);
    p->setPen(Qt::NoPen);
    p->setBrush(withAlpha(pillColor, 45));
    p->drawRoundedRect(pill, 10, 10);
    p->setFont(smallFont);
    p->setPen(pillColor.lighter(125));
    p->drawText(pill, Qt::AlignCenter, statusText);

    const QRectF titleRect = l.rtl ? QRectF(pill.right() + 8, l.text.top(), l.text.right() - pill.right() - 8, 22)
                                   : QRectF(l.text.left(), l.text.top(), pill.left() - l.text.left() - 8, 22);
    p->setFont(titleFont);
    p->setPen(Text);
    p->drawText(titleRect, startAlign(l.rtl) | Qt::AlignVCenter,
                QFontMetricsF(titleFont).elidedText(index.data(CardRole::Title).toString(), Qt::ElideRight, titleRect.width()));

    p->setFont(smallFont);
    p->setPen(Muted);
    const QRectF subtitleRect(l.text.left(), l.text.top() + 24, l.text.width(), 18);
    p->drawText(subtitleRect, startAlign(l.rtl) | Qt::AlignVCenter,
                smallMetrics.elidedText(index.data(CardRole::Subtitle).toString(), Qt::ElideRight, subtitleRect.width()));

    // شريط التشابه (بطاقة بلا تشابه، كالزائر المجهول، لا شريط لها)؛ الأزرار السريعة مكانه عند المرور
    if (!index.data(CardRole::Score).isValid() || (actions && (option.state & QStyle::State_MouseOver)))
        return;
    const double score = std::clamp(index.data(CardRole::Score).toDouble(), 0.0, 1.0);
    const QString percent = QStringLiteral("%1%").arg(qRound(score * 100));
    const double percentWidth = smallMetrics.horizontalAdvance(percent) + 6;
    const double barY = l.text.bottom() - 7;
    const QRectF track = l.rtl ? QRectF(l.text.left() + percentWidth, barY, l.text.width() - percentWidth, 5)
                               : QRectF(l.text.left(), barY, l.text.width() - percentWidth, 5);
    p->setPen(Qt::NoPen);
    p->setBrush(Border);
    p->drawRoundedRect(track, 2.5, 2.5);
    const double fillWidth = track.width() * score;
    const QRectF fill = l.rtl ? QRectF(track.right() - fillWidth, track.top(), fillWidth, track.height())
                              : QRectF(track.left(), track.top(), fillWidth, track.height());
    QLinearGradient gradient(fill.topLeft(), fill.topRight());
    gradient.setColorAt(0, color.darker(115));
    gradient.setColorAt(1, color.lighter(115));
    p->setBrush(gradient);
    p->drawRoundedRect(fill, 2.5, 2.5);
    p->setPen(Muted);
    p->drawText(l.rtl ? QRectF(l.text.left(), barY - 7, percentWidth, 18) : QRectF(track.right(), barY - 7, percentWidth, 18),
                Qt::AlignCenter, percent);
}

void FaceCardDelegate::paintGrid(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    using namespace Theme;
    const bool rtl = option.direction == Qt::RightToLeft;
    const QRectF card = QRectF(option.rect).adjusted(4, 4, -4, -4);
    paintCardBackground(p, option, card);
    const auto status = MatchStatus(index.data(CardRole::Status).toInt());
    const bool watched = index.data(CardRole::Watched).toBool();
    const QColor color = matchColor(status);
    const double size = 72 * m_scale;
    const QRectF avatar(card.center().x() - size / 2.0, card.top() + 8, size, size);
    paintAvatar(p, avatar, index.data(CardRole::Avatar).value<QPixmap>(), watched ? QColor(0xf8, 0x71, 0x71) : color);
    if (index.data(CardRole::Actions).isValid() && !(option.state & QStyle::State_MouseOver))
        paintStatusBadge(p, avatar, status, watched, rtl);

    QFont titleFont = option.font;
    titleFont.setPointSizeF(10);
    titleFont.setWeight(QFont::DemiBold);
    const QRectF titleRect(card.left() + 6, avatar.bottom() + 6, card.width() - 12, 20);
    p->setFont(titleFont);
    p->setPen(Text);
    p->drawText(titleRect, Qt::AlignCenter,
                QFontMetricsF(titleFont).elidedText(index.data(CardRole::Title).toString(), Qt::ElideRight, titleRect.width()));

    // النسبة بلون الحالة ثم الظهور
    QFont smallFont = option.font;
    smallFont.setPointSizeF(8);
    p->setFont(smallFont);
    const QFontMetricsF metrics(smallFont);
    QString line;
    if (index.data(CardRole::Score).isValid() && status != MatchStatus::Unknown)
        line = QStringLiteral("%1%").arg(qRound(std::clamp(index.data(CardRole::Score).toDouble(), 0.0, 1.0) * 100));
    const QString meta = index.data(CardRole::Subtitle).toString().section(QStringLiteral(" · "), 0, 1);
    const QRectF metaRect(card.left() + 6, titleRect.bottom() + 1, card.width() - 12, 16);
    if (!line.isEmpty()) {
        p->setPen(color.lighter(125));
        p->drawText(metaRect, Qt::AlignCenter, line);
        p->setPen(Muted);
        p->drawText(metaRect.translated(0, 15), Qt::AlignCenter, metrics.elidedText(meta, Qt::ElideRight, metaRect.width()));
    } else {
        p->setPen(Muted);
        p->drawText(metaRect, Qt::AlignCenter, metrics.elidedText(meta, Qt::ElideRight, metaRect.width()));
    }
}

QSize FaceCardDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    if (m_grid)
        return QSize(int(std::max(132.0, 124 * m_scale)), int(72 * m_scale + 72));
    return QSize(260, int(std::max(92.0, 38 + 54 * m_scale)));
}

bool FaceCardDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonDblClick) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            for (const auto &[action, rect] : actionRects(option, index)) {
                if (!rect.contains(mouse->position()))
                    continue;
                if (event->type() == QEvent::MouseButtonRelease)
                    emit actionTriggered(index.data(CardRole::Id).toInt(), action);
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool FaceCardDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (event->type() == QEvent::ToolTip) {
        for (const auto &[action, rect] : actionRects(option, index)) {
            if (rect.contains(event->pos())) {
                QToolTip::showText(event->globalPos(), actionText(action, index), view);
                return true;
            }
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

void PersonCardDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    using namespace Theme;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    const CardLayout l = layoutCard(option, 46);
    paintCardBackground(p, option, l.card);
    const bool selected = option.state & QStyle::State_Selected;
    const QPixmap avatar = index.data(CardRole::Avatar).value<QPixmap>();
    paintAvatar(p, l.avatar, avatar, selected ? Accent2 : Border);

    QFont titleFont = option.font;
    titleFont.setPointSizeF(10.5);
    titleFont.setWeight(QFont::DemiBold);
    p->setFont(titleFont);
    p->setPen(Text);
    const QRectF titleRect(l.text.left(), l.text.top() + 2, l.text.width(), 22);
    p->drawText(titleRect, startAlign(l.rtl) | Qt::AlignVCenter,
                QFontMetricsF(titleFont).elidedText(index.data(CardRole::Title).toString(), Qt::ElideRight, titleRect.width()));

    QFont smallFont = option.font;
    smallFont.setPointSizeF(8.5);
    p->setFont(smallFont);
    p->setPen(Muted);
    p->drawText(QRectF(l.text.left(), l.text.top() + 26, l.text.width(), 18), startAlign(l.rtl) | Qt::AlignVCenter,
                index.data(CardRole::Subtitle).toString());
    p->restore();
}

QSize PersonCardDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(240, 74);
}
