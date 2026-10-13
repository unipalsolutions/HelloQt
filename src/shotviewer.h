#pragma once

#include "facedatabase.h"
#include "facetypes.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QList>

#include <memory>

class FaceEngine;
class FancyButton;
class ImageView;
class QLabel;
class QListWidget;
class QSlider;
class QTimer;
class ToggleSwitch;

namespace cv {
class VideoCapture;
}

struct ShotFrame
{
    qint64 positionMs = -1;
    QImage image;
    QList<FaceItem> faces; // بإحداثيات الصورة المعروضة، مع نتيجة المطابقة
    double scale = 1.0;    // الصورة المعروضة ÷ الإطار الأصلي (أكبر من 1 بعد التحسين)
    QString error;
};

// يعرض الإطار الذي ظهر فيه وجه من الفيديو ويتنقل بين مرات ظهوره، مع تحسين الإطار وكشف
// الوجوه فيه ومطابقتها، وحفظ الإطار أو الوجه أو إضافته إلى قاعدة البيانات
class ShotViewer : public QDialog
{
    Q_OBJECT

public:
    ShotViewer(const FaceEngine *engine, const FaceDatabase *database, const QString &videoPath, const FaceItem &face,
               float threshold, bool enhance, QWidget *parent = nullptr);
    ~ShotViewer() override;

signals:
    void addFaceRequested(const FaceItem &face);
    void analyzeFromRequested(qint64 positionMs);

private:
    void seek(qint64 positionMs, bool highlightTarget = false);
    void startLoading();
    void onLoaded();
    void stepFrames(int frames);
    void jumpAppearance(int direction);
    void selectFace(int id);
    void updateDetails();
    void saveFrame();
    void saveFace();
    const FaceItem *selectedFace() const;

    const FaceEngine *m_engine;
    const FaceDatabase *m_database;
    QString m_videoPath;
    FaceItem m_face;
    float m_threshold;
    std::shared_ptr<cv::VideoCapture> m_capture;
    double m_fps = 25.0;
    qint64 m_durationMs = 0;

    QFutureWatcher<ShotFrame> m_watcher;
    qint64 m_requestedMs = -1; // الطلب الذي ينتظر انتهاء التحميل الجاري
    bool m_highlightRequested = false;
    bool m_highlightTarget = false;
    ShotFrame m_frame;
    int m_selectedId = -1;

    ImageView *m_view = nullptr;
    QSlider *m_timeline = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_details = nullptr;
    QListWidget *m_appearances = nullptr;
    ToggleSwitch *m_enhanceSwitch = nullptr;
    FancyButton *m_addButton = nullptr;
    FancyButton *m_saveFaceButton = nullptr;
    QTimer *m_scrubTimer = nullptr;
};
