#pragma once

#include "facetypes.h"

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QTimer>
#include <QWidget>

// يعرض الصورة أو الإطار مع مربعات وجوه متحركة، وحركة مسح أثناء التحليل
class ImageView : public QWidget
{
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image, bool animate = false);
    void setFaces(const QList<FaceItem> &faces);
    void setSelectedFace(int id);
    void setBusy(bool busy, const QString &text = QString());
    void clear();
    bool hasImage() const { return !m_image.isNull(); }

    // منطقة البحث عن الوجوه بإحداثيات نسبية (0..1) من الإطار، وفارغة = الإطار كله
    QRectF region() const { return m_region; }
    void setRegion(const QRectF &region);
    void setSelectingRegion(bool selecting);

signals:
    void faceClicked(int id);
    void faceDoubleClicked(int id);
    void regionSelected(const QRectF &region);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Box
    {
        int id;
        QRectF target;
        QRectF current;
        QString label;
        QColor color;
        double appear; // يبدأ سالباً لتأخير ظهور المربعات تباعاً
    };

    void tick();
    void startTicking();
    int boxAt(const QPointF &pos) const;
    QRectF imageRect(const QImage &image) const;
    QRectF mapToView(const QRectF &rect) const;
    void paintEmpty(QPainter &p, double t);
    void paintBusy(QPainter &p, double t);
    void paintBox(QPainter &p, const Box &box, double t);
    void paintRegion(QPainter &p);
    QPointF toNormalized(const QPointF &pos) const;

    QImage m_image;
    QImage m_previous;
    double m_fade = 1.0;
    QList<Box> m_boxes;
    int m_selected = -1;
    int m_hovered = -1;
    bool m_busy = false;
    QString m_busyText;
    QRectF m_region;
    bool m_selectingRegion = false;
    bool m_dragging = false;
    QPointF m_dragStart;
    QPointF m_dragEnd;
    QTimer m_timer;
    QElapsedTimer m_clock;
};
