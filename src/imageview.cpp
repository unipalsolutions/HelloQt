#include "imageview.h"

#include <QMouseEvent>
#include <QPainter>

ImageView::ImageView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ImageView::setImage(const QImage &image)
{
    m_image = image;
    update();
}

void ImageView::setFaces(const QList<FaceItem> &faces)
{
    m_boxes.clear();
    for (const FaceItem &face : faces) {
        QString label = matchLabel(face.match);
        if (face.match.status != MatchStatus::Unknown)
            label += QStringLiteral(" %1%").arg(qRound(face.match.score * 100));
        m_boxes.append({face.id, face.box, label, matchColor(face.match.status)});
    }
    update();
}

void ImageView::setSelectedFace(int id)
{
    m_selected = id;
    update();
}

void ImageView::clear()
{
    m_image = QImage();
    m_boxes.clear();
    m_selected = -1;
    update();
}

QRectF ImageView::imageRect() const
{
    if (m_image.isNull())
        return {};
    QSizeF size = m_image.size();
    size.scale(this->size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
}

QRectF ImageView::mapToView(const QRectF &rect) const
{
    const QRectF target = imageRect();
    const double scale = target.width() / m_image.width();
    return QRectF(target.x() + rect.x() * scale, target.y() + rect.y() * scale,
                  rect.width() * scale, rect.height() * scale);
}

void ImageView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(32, 33, 36));

    if (m_image.isNull()) {
        painter.setPen(QColor(170, 170, 170));
        QFont font = painter.font();
        font.setPointSize(16);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("افتح صورة أو فيديو أو شغّل الكاميرا للبدء"));
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawImage(imageRect(), m_image);

    QFont font = painter.font();
    font.setPointSize(11);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);

    for (const Box &box : m_boxes) {
        const QRectF r = mapToView(box.rect);
        painter.setPen(QPen(box.color, box.id == m_selected ? 4 : 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(r);

        const QSizeF textSize(metrics.horizontalAdvance(box.label) + 12, metrics.height() + 4);
        QRectF labelRect(QPointF(r.center().x() - textSize.width() / 2.0, r.top() - textSize.height()), textSize);
        if (labelRect.top() < 0)
            labelRect.moveTop(r.bottom());
        painter.setPen(Qt::NoPen);
        painter.setBrush(box.color);
        painter.drawRoundedRect(labelRect, 4, 4);
        painter.setPen(Qt::white);
        painter.drawText(labelRect, Qt::AlignCenter, box.label);
    }
}

void ImageView::mousePressEvent(QMouseEvent *event)
{
    for (const Box &box : m_boxes) {
        if (mapToView(box.rect).contains(event->position())) {
            emit faceClicked(box.id);
            return;
        }
    }
    QWidget::mousePressEvent(event);
}
