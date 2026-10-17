#pragma once

#include "facetypes.h"

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QSet>
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
    QImage image() const { return m_image; }

    // منطقة التحليل بإحداثيات نسبية (0 إلى 1)؛ ما خارجها يُعتَّم
    void setRegion(const QRectF &region);
    QRectF region() const { return m_region; }
    // وضع الرسم: سحب الفأرة يحدد منطقة جديدة
    void setSelectingRegion(bool selecting);
    bool isSelectingRegion() const { return m_selecting; }

    // التكبير والتحريك: عجلة الفأرة تكبّر حول المؤشر، والسحب يحرّك الصورة المكبّرة
    double zoom() const { return m_zoom; }
    void zoomBy(double factor, const QPointF &anchor = QPointF(-1, -1));
    void resetZoom();
    void zoomToFace(int id);
    int faceAt(const QPointF &pos) const { return boxAt(pos); }

    // إظهار أسماء الوجوه فوق المربعات أو إخفاؤها في الصور المزدحمة
    void setLabelsVisible(bool visible);
    // أشخاص قائمة المراقبة: مربعاتهم تنبض وتحمل علامة تنبيه
    void setWatched(const QSet<QString> &personIds);
    bool labelsVisible() const { return m_showLabels; }

    // التنقل بين صور المجلد: سهمان على جانبي الصورة وعنوان صغير بالموضع
    void setNavigation(bool canPrevious, bool canNext, const QString &caption);

    // الصورة كما تظهر مع مربعات الوجوه وأسمائها، لحفظها أو نسخها
    QImage annotatedImage() const;

signals:
    void faceClicked(int id);
    void faceDoubleClicked(int id);
    void regionSelected(const QRectF &region);
    void navigateRequested(int step); // -1 السابقة، 1 التالية
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Box
    {
        int id;
        QRectF target;
        QRectF current;
        QString label;
        QColor color;
        double appear;
        bool watched = false; // يبدأ سالباً لتأخير ظهور المربعات تباعاً
    };

    void tick();
    void startTicking();
    int boxAt(const QPointF &pos) const;
    QRectF fitRect(const QImage &image) const;
    QRectF imageRect() const;
    void setZoom(double zoom, const QPointF &anchor);
    void clampPan();
    QRectF navButton(int step) const;
    void paintNavigation(QPainter &p);
    void paintZoomBadge(QPainter &p);
    QRectF mapToView(const QRectF &rect) const;
    void paintEmpty(QPainter &p, double t);
    void paintBusy(QPainter &p, double t);
    void paintBox(QPainter &p, const Box &box, double t);
    void paintRegion(QPainter &p);
    QPointF toImageFraction(const QPointF &pos) const;

    QImage m_image;
    QImage m_previous;
    // الصورة مصغّرة بحجم عرضها: الرسم المتكرر (نبض الوجه المحدد، المربعات) لا يعيد تصغير الإطار كاملاً
    QImage m_scaled;
    qint64 m_scaledKey = 0;
    double m_fade = 1.0;
    QList<Box> m_boxes;
    int m_selected = -1;
    int m_hovered = -1;
    bool m_busy = false;
    QRectF m_region;
    bool m_selecting = false;
    bool m_dragging = false;
    QPointF m_dragStart;
    QPointF m_dragEnd;
    QString m_busyText;
    double m_zoom = 1.0;
    QPointF m_pan; // إزاحة مركز الصورة المكبّرة عن مركز العرض
    bool m_panning = false;
    QPointF m_panStart;
    QPointF m_panOrigin;
    bool m_showLabels = true;
    QSet<QString> m_watched;
    QList<FaceItem> m_faces;
    bool m_canPrevious = false;
    bool m_canNext = false;
    QString m_caption;
    int m_navHover = 0;
    QTimer m_timer;
    QElapsedTimer m_clock;
};
