#pragma once

#include "facedatabase.h"
#include "recorder.h"

#include <QDialog>
#include <QList>
#include <QSet>
#include <QWidget>

class FancyButton;
class QComboBox;
class QDateEdit;
class QLabel;
class QListWidget;

// خط زمني ليوم كامل: صف لكل كاميرا فيه فترات تسجيلها وعلامات الظهور
class DayTimeline : public QWidget
{
    Q_OBJECT

public:
    struct Marker
    {
        QDateTime time;
        QColor color;
        int index = -1; // رقمه في قائمة الظهور
    };
    struct Lane
    {
        QString name;
        QList<CameraRecorder::RecordingFile> recordings;
        QList<Marker> markers;
    };

    explicit DayTimeline(QWidget *parent = nullptr);
    void setDay(const QDate &day, const QList<Lane> &lanes);
    void setSelected(int index);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void markerClicked(int index);
    // نقر مزدوج على فترة تسجيل: فتحها عند تلك اللحظة
    void recordingActivated(const QString &path, qint64 positionMs);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    QRectF plotRect() const;
    QRectF laneRect(int lane) const;
    double xOf(const QDateTime &time) const;
    QDateTime timeAt(double x) const;
    int laneAt(const QPointF &point) const;
    const Marker *markerAt(const QPointF &point) const;

    QDate m_day;
    QList<Lane> m_lanes;
    int m_selected = -1;
};

// مستعرض التسجيلات: يوم بيوم، لكل كاميرا تسجيلاتها ومن ظهر أمامها ومتى، والبحث عن شخص
// («أين ظهر فلان هذا الأسبوع»)، وفتح التسجيل عند لحظة الظهور
class RecordingsDialog : public QDialog
{
    Q_OBJECT

public:
    RecordingsDialog(const FaceDatabase *database, const QSet<QString> &watched, QWidget *parent = nullptr);

signals:
    void openFileRequested(const QString &path, qint64 positionMs);

private:
    struct Entry
    {
        Sighting sighting;
        QString personId; // فارغ للزائر المجهول
        QString name;
        bool watched = false;
        QString lane;
    };

    void reload();
    void fillPersons();
    void select(int index);
    void openEntry(int index);
    QString laneOf(const QString &camera) const;

    const FaceDatabase *m_database;
    QSet<QString> m_watched;
    QDateEdit *m_date;
    QComboBox *m_camera;
    QComboBox *m_person;
    QLabel *m_summary;
    DayTimeline *m_timeline;
    QListWidget *m_list;
    FancyButton *m_open;
    FancyButton *m_snapshot;
    FancyButton *m_clip;
    QList<Entry> m_entries;
};
