#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QWidget>

// مسار شخص في الخط الزمني: صورته واسمه وفترات ظهوره
struct TimelineLane
{
    int id = 0;
    QString name;
    QColor color;
    QPixmap avatar;
    QList<QPair<qint64, qint64>> segments;
};

// خط زمني بأسلوب برامج المونتاج: مسطرة الزمن، شريط مصغرات الإطارات، مؤشر الموضع،
// ومسار لكل شخص تظهر عليه فترات ظهوره. Ctrl+العجلة للتكبير، Shift+العجلة أو السحب بالزر الأوسط للتنقل.
// مقبضا الاقتطاع (الأصفران) يُسحبان لتحديد الجزء المطلوب، أو I و O عند المؤشر، والزر الأيمن يفتح قائمة العمليات.
class TimelineView : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineView(QWidget *parent = nullptr);

    void clear();
    void setDuration(qint64 durationMs);
    qint64 duration() const { return m_duration; }
    void setFps(double fps) { m_fps = fps > 1.0 ? fps : 25.0; }
    // الجزء المطلوب تحليله؛ ما خارجه يُعتَّم
    void setRange(qint64 startMs, qint64 endMs);
    // ما وصل إليه التحليل (سالب = اكتمل)
    void setAnalyzedUntil(qint64 ms);
    void setPosition(qint64 ms);
    qint64 position() const { return m_position; }
    void addThumbnail(qint64 ms, const QImage &frame);
    const QMap<qint64, QImage> &thumbnails() const { return m_thumbnails; }
    void setThumbnails(const QMap<qint64, QImage> &thumbnails);
    bool wantsThumbnail(qint64 ms) const; // هل هذا الموضع بعيد بما يكفي عن آخر مصغرة؟
    void setLanes(const QList<TimelineLane> &lanes);
    void setSelectedLane(int id);
    void setLabelWidth(int width);

    void zoomIn();
    void zoomOut();
    void fit();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void seekRequested(qint64 ms);
    void laneClicked(int id, qint64 ms);   // ms سالب عند النقر على اسم المسار
    void laneActivated(int id, qint64 ms); // نقر مزدوج
    void rangeEdited(qint64 startMs, qint64 endMs); // اقتطاع بالسحب أو بالمفاتيح (0 = بلا حد)
    void contextMenuRequested(const QPoint &globalPos, qint64 ms, int laneId); // laneId سالب خارج المسارات

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    double trackLeft() const { return m_labelWidth; }
    double trackWidth() const { return std::max(10.0, width() - m_labelWidth - 8.0); }
    double xOf(qint64 ms) const;
    qint64 timeAt(double x) const;
    int laneAt(double y) const;
    int lanesTop() const;
    int visibleLaneCount() const;
    void setZoom(double msPerPixel, double anchorX);
    void clampView();
    void ensurePlayheadVisible();
    int handleAt(const QPointF &pos) const; // 1 = بداية الاقتطاع، 2 = نهايته، 0 = لا شيء
    void editRange(qint64 startMs, qint64 endMs);

    qint64 m_duration = 0;
    double m_fps = 25.0;
    qint64 m_rangeStart = 0;
    qint64 m_rangeEnd = 0;
    qint64 m_analyzedUntil = -1;
    qint64 m_position = -1;
    QMap<qint64, QImage> m_thumbnails;
    QList<TimelineLane> m_lanes;
    int m_selectedLane = -1;
    int m_labelWidth = 150;

    double m_msPerPixel = 0.0; // 0 = ملاءمة الفيديو كله للعرض
    double m_viewStart = 0.0;  // زمن الحافة اليسرى
    int m_laneScroll = 0;      // أول مسار ظاهر

    bool m_scrubbing = false;
    bool m_panning = false;
    bool m_rightPressed = false;
    int m_dragHandle = 0;
    QPointF m_panOrigin;
    double m_panStart = 0.0;
    double m_hoverX = -1.0;
};
