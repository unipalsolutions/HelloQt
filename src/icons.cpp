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
    case Name::Region:
        line(3, 8, 3, 3);
        line(3, 3, 8, 3);
        line(16, 3, 21, 3);
        line(21, 3, 21, 8);
        line(21, 16, 21, 21);
        line(21, 21, 16, 21);
        line(8, 21, 3, 21);
        line(3, 21, 3, 16);
        p.addRoundedRect(QRectF(8, 8, 8, 8), 1.5, 1.5);
        break;
    case Name::Clock:
        p.addEllipse(QPointF(12, 12), 9, 9);
        line(12, 7, 12, 12);
        line(12, 12, 15.5, 14);
        break;
    case Name::Merge:
        line(6, 3, 6, 9);
        p.moveTo(18, 3);
        p.lineTo(18, 9);
        p.moveTo(6, 9);
        p.cubicTo(6, 13, 12, 13, 12, 16);
        p.moveTo(18, 9);
        p.cubicTo(18, 13, 12, 13, 12, 16);
        line(12, 16, 12, 21);
        line(9, 18.5, 12, 21.5);
        line(15, 18.5, 12, 21.5);
        break;
    case Name::Film:
        p.addRoundedRect(QRectF(3, 4, 18, 16), 2.5, 2.5);
        line(7, 4, 7, 20);
        line(17, 4, 17, 20);
        line(3, 9, 7, 9);
        line(3, 15, 7, 15);
        line(17, 9, 21, 9);
        line(17, 15, 21, 15);
        break;
    case Name::Next:
        p.moveTo(6, 6);
        p.lineTo(14, 12);
        p.lineTo(6, 18);
        p.closeSubpath();
        line(18, 6, 18, 18);
        break;
    case Name::Previous:
        p.moveTo(18, 6);
        p.lineTo(10, 12);
        p.lineTo(18, 18);
        p.closeSubpath();
        line(6, 6, 6, 18);
        break;
    case Name::Sliders:
        line(4, 7, 20, 7);
        line(4, 17, 20, 17);
        p.addEllipse(QPointF(9, 7), 2.2, 2.2);
        p.addEllipse(QPointF(15, 17), 2.2, 2.2);
        break;
    case Name::ZoomIn:
        p.addEllipse(QPointF(10.5, 10.5), 6.5, 6.5);
        line(15.5, 15.5, 21, 21);
        line(7.5, 10.5, 13.5, 10.5);
        line(10.5, 7.5, 10.5, 13.5);
        break;
    case Name::ZoomOut:
        p.addEllipse(QPointF(10.5, 10.5), 6.5, 6.5);
        line(15.5, 15.5, 21, 21);
        line(7.5, 10.5, 13.5, 10.5);
        break;
    case Name::FitWidth:
        line(3, 5, 3, 19);
        line(21, 5, 21, 19);
        line(7, 12, 17, 12);
        line(7, 12, 10, 9);
        line(7, 12, 10, 15);
        line(17, 12, 14, 9);
        line(17, 12, 14, 15);
        break;
    case Name::Compare:
        p.addEllipse(QPointF(8.5, 12), 6.5, 6.5);
        p.addEllipse(QPointF(15.5, 12), 6.5, 6.5);
        break;
    case Name::Collect:
        p.addRoundedRect(QRectF(3, 4, 13, 11), 2, 2);
        line(3, 12, 7, 9);
        line(7, 9, 11, 13);
        line(18, 10, 18, 20);
        line(18, 20, 15, 17);
        line(18, 20, 21, 17);
        break;
    case Name::Star:
        p.moveTo(12, 3);
        p.lineTo(14.6, 8.6);
        p.lineTo(20.6, 9.2);
        p.lineTo(16, 13.2);
        p.lineTo(17.4, 19.2);
        p.lineTo(12, 16);
        p.lineTo(6.6, 19.2);
        p.lineTo(8, 13.2);
        p.lineTo(3.4, 9.2);
        p.lineTo(9.4, 8.6);
        p.closeSubpath();
        break;
    case Name::Export:
        p.moveTo(14, 3);
        p.lineTo(6, 3);
        p.quadTo(4, 3, 4, 5);
        p.lineTo(4, 19);
        p.quadTo(4, 21, 6, 21);
        p.lineTo(18, 21);
        p.quadTo(20, 21, 20, 19);
        p.lineTo(20, 9);
        p.lineTo(14, 3);
        p.lineTo(14, 9);
        p.lineTo(20, 9);
        line(12, 12, 12, 18);
        line(12, 18, 9.5, 15.5);
        line(12, 18, 14.5, 15.5);
        break;
    case Name::History:
        p.moveTo(4, 12);
        p.arcTo(QRectF(4, 3, 18, 18), 180, -300);
        line(4, 12, 2, 9.5);
        line(4, 12, 6.5, 10);
        line(13, 8, 13, 12);
        line(13, 12, 16, 14);
        break;
    case Name::Copy:
        p.addRoundedRect(QRectF(8, 8, 12, 12), 2, 2);
        p.moveTo(16, 8);
        p.lineTo(16, 5);
        p.quadTo(16, 4, 15, 4);
        p.lineTo(5, 4);
        p.quadTo(4, 4, 4, 5);
        p.lineTo(4, 15);
        p.quadTo(4, 16, 5, 16);
        p.lineTo(8, 16);
        break;
    case Name::Record:
        p.addEllipse(QPointF(12, 12), 6.5, 6.5);
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
    painter.setBrush(name == Name::Stop || name == Name::Record ? QBrush(color) : QBrush(Qt::NoBrush));
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
