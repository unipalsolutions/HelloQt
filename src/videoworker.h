#pragma once

#include "facedatabase.h"
#include "facetypes.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>

#include <atomic>

class FaceEngine;

// يحلل الفيديو أو الكاميرا في خيط منفصل ويتتبع كل وجه عبر الإطارات
class VideoWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoWorker(const FaceEngine *engine, QObject *parent = nullptr);

    // آمنة للاستدعاء من أي خيط
    void setGallery(GalleryPtr gallery);
    void setThreshold(float threshold);
    void setFrameStep(int step); // 0: تلقائي حسب سرعة إطارات الفيديو
    void requestStop();
    void frameShown();

public slots:
    void process(const QString &source); // مسار ملف أو "camera:0"

signals:
    void started(int frameStep, double fps);
    void frameReady(const QImage &frame, const QList<FaceItem> &faces, int frameIndex, int frameCount);
    void trackUpdated(const FaceItem &track);
    void finished(const QString &error);

private:
    const FaceEngine *m_engine;
    QMutex m_mutex;
    GalleryPtr m_gallery;
    float m_threshold = 0.30f;
    std::atomic<int> m_frameStep{0};
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_pendingFrames{0};
};
