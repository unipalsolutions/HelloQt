#pragma once

#include "camerarules.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QWidget>

class QCheckBox;
class QLabel;
class QRadioButton;
class QTimeEdit;

// لوحة الكادر: سحب بالفأرة يرسم منطقة، والنقر بالزر الأيمن على منطقة يحذفها
class ZoneCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit ZoneCanvas(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void setZones(const QList<QRectF> &zones);
    QList<QRectF> zones() const { return m_zones; }
    void setExclude(bool exclude);
    void setMessage(const QString &message);

    QSize sizeHint() const override { return {640, 360}; }

signals:
    void zonesChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QRectF imageRect() const;
    QPointF toNormalized(const QPointF &point) const;

    QImage m_image;
    QString m_message;
    QList<QRectF> m_zones;
    bool m_exclude = false;
    bool m_dragging = false;
    QPointF m_start;
    QPointF m_current;
};

// قواعد كاميرا واحدة: المناطق، وساعات التنبيه، وساعات التسجيل
class CameraRulesDialog : public QDialog
{
    Q_OBJECT

public:
    // frame صورة من الكاميرا إن كانت تعمل؛ وإلا تُلتقط صورة منها في الخلفية
    CameraRulesDialog(const QString &source, const QString &cameraName, const QImage &frame, QWidget *parent = nullptr);
    ~CameraRulesDialog() override;

    CameraRules::Rules rules() const;

    void accept() override;

private:
    void updateSummary();

    QString m_source;
    ZoneCanvas *m_canvas;
    QRadioButton *m_include;
    QRadioButton *m_exclude;
    QCheckBox *m_alertsEnabled;
    QTimeEdit *m_alertsFrom;
    QTimeEdit *m_alertsTo;
    QCheckBox *m_recordEnabled;
    QTimeEdit *m_recordFrom;
    QTimeEdit *m_recordTo;
    QLabel *m_summary;
    QFutureWatcher<QImage> m_snapshot;
};
