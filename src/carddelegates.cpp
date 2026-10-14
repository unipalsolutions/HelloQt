#include "carddelegates.h"

#include "facetypes.h"
#include "icons.h"
#include "theme.h"

#include <QPainter>
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

void FaceCardDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    using namespace Theme;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    const CardLayout l = layoutCard(option, 54);
    paintCardBackground(p, option, l.card);

    const auto status = MatchStatus(index.data(CardRole::Status).toInt());
    const QColor color = matchColor(status);
    paintAvatar(p, l.avatar, index.data(CardRole::Avatar).value<QPixmap>(), color);

    // الاسم والحالة
    QFont titleFont = option.font;
    titleFont.setPointSizeF(11);
    titleFont.setWeight(QFont::DemiBold);
    QFont smallFont = option.font;
    smallFont.setPointSizeF(8.5);

    const QString customStatus = index.data(CardRole::StatusText).toString();
    const QString statusText = !customStatus.isEmpty()              ? customStatus
                             : status == MatchStatus::Confirmed ? QStringLiteral("مؤكد")
                             : status == MatchStatus::Possible  ? QStringLiteral("محتمل")
                                                                : QStringLiteral("غير مسجل");
    const QFontMetricsF smallMetrics(smallFont);
    const double pillWidth = smallMetrics.horizontalAdvance(statusText) + 16;
    const QRectF pill(l.rtl ? l.text.left() : l.text.right() - pillWidth, l.text.top() + 1, pillWidth, 20);
    p->setPen(Qt::NoPen);
    p->setBrush(withAlpha(color, 45));
    p->drawRoundedRect(pill, 10, 10);
    p->setFont(smallFont);
    p->setPen(color.lighter(125));
    p->drawText(pill, Qt::AlignCenter, statusText);

    const QRectF titleRect = l.rtl ? QRectF(pill.right() + 8, l.text.top(), l.text.right() - pill.right() - 8, 22)
                                   : QRectF(l.text.left(), l.text.top(), pill.left() - l.text.left() - 8, 22);
    p->setFont(titleFont);
    p->setPen(Text);
    p->drawText(titleRect, startAlign(l.rtl) | Qt::AlignVCenter,
                QFontMetricsF(titleFont).elidedText(index.data(CardRole::Title).toString(), Qt::ElideRight, titleRect.width()));

    p->setFont(smallFont);
    p->setPen(Muted);
    p->drawText(QRectF(l.text.left(), l.text.top() + 24, l.text.width(), 18), startAlign(l.rtl) | Qt::AlignVCenter,
                index.data(CardRole::Subtitle).toString());

    // شريط التشابه
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

    p->restore();
}

QSize FaceCardDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(260, 92);
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
