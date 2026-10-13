#pragma once

#include "facetypes.h"

#include <QImage>
#include <QList>
#include <QWidget>

// يعرض الصورة أو الإطار مع مربعات الوجوه وأسمائها
class ImageView : public QWidget
{
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void setFaces(const QList<FaceItem> &faces);
    void setSelectedFace(int id);
    void clear();

signals:
    void faceClicked(int id);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    struct Box
    {
        int id;
        QRectF rect;
        QString label;
        QColor color;
    };

    QRectF imageRect() const;
    QRectF mapToView(const QRectF &rect) const;

    QImage m_image;
    QList<Box> m_boxes;
    int m_selected = -1;
};
