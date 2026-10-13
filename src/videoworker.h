#pragma once

#include "facedatabase.h"
#include "facetypes.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QRectF>

#include <utility>
#include <vector>

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
    void setRange(qint64 startMs, qint64 endMs); // الجزء المطلوب من الفيديو؛ endMs = 0 حتى النهاية
    void setRegion(const QRectF &region);        // منطقة البحث بإحداثيات نسبية؛ فارغة = الإطار كله
    void setEnhance(bool enhance);               // تحسين كل إطار قبل كشف الوجوه
    void mergeIdentities(int keepId, int removedId); // دمج طلبه المستخدم أثناء التحليل
    void requestStop();
    void setPaused(bool paused); // إيقاف مؤقت دون فقدان التقدم أو الأشخاص المكتشفين
    bool isPaused() const { return m_paused; }
    void frameShown();

public slots:
    void process(const QString &source); // مسار ملف أو "camera:0"

signals:
    void started(int frameStep, double fps);
    void frameReady(const QImage &frame, const QList<FaceItem> &faces, int frameIndex, int frameCount, qint64 positionMs);
    void trackUpdated(const FaceItem &track);
    void identitiesMerged(int keepId, int removedId); // دمج تلقائي لشخص ظُن أنه شخصان
    void finished(const QString &error);

private:
    const FaceEngine *m_engine;
    QMutex m_mutex;
    GalleryPtr m_gallery;
    float m_threshold = 0.30f;
    qint64 m_startMs = 0;
    qint64 m_endMs = 0;
    QRectF m_region;
    std::vector<std::pair<int, int>> m_pendingMerges;
    std::atomic<bool> m_enhance{false};
    std::atomic<int> m_frameStep{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int> m_pendingFrames{0};
};
