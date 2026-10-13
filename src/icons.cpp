#include "icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace Icons {

namespace {

// كل الأيقونات معرّفة على شبكة 24x24
QPainterPath outline(Name name)
{
    QPainterPath p;
    auto line = [&p](qreal x1, qreal y1, qreal x2, qreal y2) {
        p.moveTo(x1, y1);
        p.lineTo(x2, y2);
    };
    auto topArc = [&p](const QRectF &r) {
        p.arcMoveTo(r, 180);
        p.arcTo(r, 180, -180);
    };

    switch (name) {
    case Name::Image:
        p.addRoundedRect(QRectF(3, 5, 18, 14), 2.5, 2.5);
        p.addEllipse(QPointF(8.5, 10), 1.6, 1.6);
        p.moveTo(21, 16);
        p.lineTo(15.5, 10.5);
        p.lineTo(5, 19);
        break;
    case Name::Video:
        p.addRoundedRect(QRectF(2, 6, 14, 12), 2.5, 2.5);
        p.moveTo(16, 10);
        p.lineTo(22, 7);
        p.lineTo(22, 17);
        p.lineTo(16, 14);
        break;
    case Name::Camera:
        p.addRoundedRect(QRectF(2, 7, 20, 13), 2.5, 2.5);
        p.moveTo(8, 7);
        p.lineTo(9.5, 4.5);
        p.lineTo(14.5, 4.5);
        p.lineTo(16, 7);
        p.addEllipse(QPointF(12, 13.5), 3.5, 3.5);
        break;
    case Name::Stop:
        p.addRoundedRect(QRectF(6, 6, 12, 12), 2.5, 2.5);
        break;
    case Name::Pause:
        line(9, 6, 9, 18);
        line(15, 6, 15, 18);
        break;
    case Name::Play:
        p.moveTo(8, 5.5);
        p.lineTo(18.5, 12);
        p.lineTo(8, 18.5);
        p.closeSubpath();
        break;
    case Name::Sparkle:
        p.moveTo(10, 3);
        p.quadTo(10.8, 9.2, 17, 10);
        p.quadTo(10.8, 10.8, 10, 17);
        p.quadTo(9.2, 10.8, 3, 10);
        p.quadTo(9.2, 9.2, 10, 3);
        line(18, 15, 18, 21);
        line(15, 18, 21, 18);
        break;
    case Name::Save:
        line(12, 4, 12, 15);
        p.moveTo(7, 10);
        p.lineTo(12, 15);
        p.lineTo(17, 10);
        line(5, 20, 19, 20);
        break;
    case Name::UserPlus:
        p.addEllipse(QPointF(9, 8), 4, 4);
        topArc(QRectF(2, 14, 14, 14));
        line(19, 8, 19, 14);
        line(16, 11, 22, 11);
        break;
    case Name::Edit:
        p.moveTo(4, 20);
        p.lineTo(4, 16);
        p.lineTo(15, 5);
        p.lineTo(19, 9);
        p.lineTo(8, 20);
        p.closeSubpath();
        line(13, 7, 17, 11);
        break;
    case Name::Trash:
        line(4, 7, 20, 7);
        p.moveTo(6, 7);
        p.lineTo(7, 20);
        p.lineTo(17, 20);
        p.lineTo(18, 7);
        p.moveTo(9, 7);
        p.lineTo(9, 4);
        p.lineTo(15, 4);
        p.lineTo(15, 7);
        line(10, 11, 10, 16);
        line(14, 11, 14, 16);
        break;
    case Name::Folder:
        p.moveTo(3, 6);
        p.lineTo(9, 6);
        p.lineTo(11, 8);
        p.lineTo(21, 8);
        p.lineTo(21, 19);
        p.lineTo(3, 19);
        p.closeSubpath();
        break;
    case Name::PhotoAdd:
        p.addRoundedRect(QRectF(3, 7, 14, 13), 2.5, 2.5);
        p.addEllipse(QPointF(7.5, 11.5), 1.4, 1.4);
        p.moveTo(17, 17);
        p.lineTo(13, 13);
        p.lineTo(5, 20);
        line(20, 2, 20, 8);
        line(17, 5, 23, 5);
        break;
    case Name::Search:
        p.addEllipse(QPointF(11, 11), 6, 6);
        line(15.5, 15.5, 21, 21);
        break;
    case Name::Scan:
        p.moveTo(3, 8);
        p.lineTo(3, 3);
        p.lineTo(8, 3);
        p.moveTo(16, 3);
        p.lineTo(21, 3);
        p.lineTo(21, 8);
        p.moveTo(21, 16);
        p.lineTo(21, 21);
        p.lineTo(16, 21);
        p.moveTo(8, 21);
        p.lineTo(3, 21);
        p.lineTo(3, 16);
        p.addEllipse(QPointF(12, 10), 2.8, 2.8);
        p.moveTo(7, 17.5);
        p.cubicTo(8.5, 14, 15.5, 14, 17, 17.5);
        break;
    case Name::Users:
        p.addEllipse(QPointF(9, 8), 3.5, 3.5);
        topArc(QRectF(2.5, 14, 13, 13));
        p.addEllipse(QPointF(17, 9), 2.6, 2.6);
        p.arcMoveTo(QRectF(14, 14.5, 8, 9), 90);
        p.arcTo(QRectF(14, 14.5, 8, 9), 90, -90);
        break;
    case Name::Lock:
        p.addRoundedRect(QRectF(5, 11, 14, 10), 2.5, 2.5);
        p.moveTo(8, 11);
        p.lineTo(8, 8);
        p.arcTo(QRectF(8, 4, 8, 8), 180, -180);
        p.lineTo(16, 11);
        break;
    case Name::Check:
        p.moveTo(5, 12.5);
        p.lineTo(10, 17);
        p.lineTo(19, 7);
        break;
    case Name::Alert:
        p.moveTo(12, 3.5);
        p.lineTo(21.5, 20);
        p.lineTo(2.5, 20);
        p.closeSubpath();
        line(12, 9.5, 12, 14);
        p.addEllipse(QPointF(12, 17), 0.3, 0.3);
        break;
    case Name::Info:
        p.addEllipse(QPointF(12, 12), 9, 9);
        line(12, 11, 12, 16.5);
        p.addEllipse(QPointF(12, 7.8), 0.3, 0.3);
        break;
    }
    return p;
}

} // namespace

void paint(QPainter &painter, Name name, const QRectF &rect, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(rect.topLeft());
    painter.scale(rect.width() / 24.0, rect.height() / 24.0);
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(name == Name::Stop ? QBrush(color) : QBrush(Qt::NoBrush));
    painter.drawPath(outline(name));
    painter.restore();
}

QIcon icon(Name name, const QColor &color, int size)
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(2.0);
    QPainter painter(&pixmap);
    paint(painter, name, QRectF(0, 0, size, size), color);
    return QIcon(pixmap);
}

} // namespace Icons
