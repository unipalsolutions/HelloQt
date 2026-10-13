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
    QList<FaceItem> faces;
    double scale = 1.0; // حجم الصورة المعروضة ÷ حجم الإطار الأصلي (يتغير مع التحسين)
    QString error;
};

// يعرض لقطات وجه من الفيديو: أوضح لقطة، والتنقل بين مرات ظهوره إطاراً إطاراً، مع التحسين والحفظ
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
    const FaceItem *selectedFace() const;
    void updateDetails();
    void saveFrame();
    void saveFace();

    const FaceEngine *m_engine;
    const FaceDatabase *m_database;
    QString m_videoPath;
    FaceItem m_face;
    float m_threshold;
    std::shared_ptr<cv::VideoCapture> m_capture;
    double m_fps = 25.0;
    qint64 m_durationMs = 0;

    QFutureWatcher<ShotFrame> m_watcher;
    ShotFrame m_frame;
    qint64 m_requestedMs = -1;    // طلب ينتظر انتهاء التحميل الجاري
    bool m_highlightRequested = false;
    bool m_highlightTarget = false;
    int m_selectedId = -1;

    ImageView *m_view = nullptr;
    QSlider *m_timeline = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_details = nullptr;
    QListWidget *m_appearances = nullptr;
    ToggleSwitch *m_enhanceSwitch = nullptr;
    FancyButton *m_saveFaceButton = nullptr;
    FancyButton *m_addButton = nullptr;
    QTimer *m_scrubTimer = nullptr;
};
