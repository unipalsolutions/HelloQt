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

signals:
    void faceClicked(int id);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
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

    QImage m_image;
    QImage m_previous;
    double m_fade = 1.0;
    QList<Box> m_boxes;
    int m_selected = -1;
    int m_hovered = -1;
    bool m_busy = false;
    QString m_busyText;
    QTimer m_timer;
    QElapsedTimer m_clock;
};
